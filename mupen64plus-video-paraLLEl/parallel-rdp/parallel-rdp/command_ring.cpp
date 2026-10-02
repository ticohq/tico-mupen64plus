/* Copyright (c) 2020 Themaister
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <chrono>
#include "command_ring.hpp"
#include <algorithm>
#include <string.h>
#include "rdp_device.hpp"
#include "thread_id.hpp"
#include <assert.h>

namespace RDP
{
void CommandRing::init(
#ifdef PARALLEL_RDP_SHADER_DIR
		Granite::Global::GlobalManagersHandle global_handles_,
#endif
		CommandProcessor *processor_, unsigned count)
{
	assert((count & (count - 1)) == 0);
	teardown_thread();
	processor = processor_;
	ring.resize(count);
	write_count = 0;
	read_count = 0;
	completed_count = 0;
#ifdef PARALLEL_RDP_SHADER_DIR
	global_handles = std::move(global_handles_);
#endif
	thr = std::thread(&CommandRing::thread_loop, this);
}

void CommandRing::teardown_thread()
{
	if (thr.joinable())
	{
		enqueue_command(0, nullptr);
		thr.join();
	}
}

CommandRing::~CommandRing()
{
	teardown_thread();
}

// The waiter sets its flag and then checks its condition under the lock; the
// other side changes the counter and then checks the flag (all seq_cst), so at
// least one of them sees the other. Taking the lock before notifying means the
// waiter is either before its check or already inside cond.wait.
void CommandRing::wake_if(const std::atomic<bool> &waiting)
{
	if (waiting.load())
	{
		{
			std::lock_guard<std::mutex> holder{lock};
		}
		cond.notify_all();
	}
}

void CommandRing::drain()
{
	if (completed_count.load() == write_count.load())
		return;
	std::unique_lock<std::mutex> holder{lock};
	drain_waiting = true;
	cond.wait(holder, [this]() {
		return completed_count.load() == write_count.load();
	});
	drain_waiting = false;
}

// Single producer (the emulation thread) and single consumer (thread_loop).
uint64_t CommandRing::wait_for_space(size_t num_words)
{
	const uint64_t wc = write_count.load(std::memory_order_relaxed);
	uint64_t rc = read_count.load();
	if (wc + num_words <= rc + ring.size())
		return rc;

	// Ring full: sleep until the consumer frees space.
	std::unique_lock<std::mutex> holder{lock};
	producer_waiting = true;
	cond.wait(holder, [&]() {
		rc = read_count.load();
		return wc + num_words <= rc + ring.size();
	});
	producer_waiting = false;
	return rc;
}

void CommandRing::publish(uint64_t new_write_count)
{
	write_count.store(new_write_count);
	// No lock here: if the consumer is between its check and cond.wait, the
	// notify is missed, but its wait times out after 500 us. Locking made the
	// emulation thread contend with the consumer's 500 us idle wakeups.
	if (consumer_sleeping.load())
		cond.notify_all();
	// drain() also waits for write_count == completed_count.
	wake_if(drain_waiting);
}

void CommandRing::enqueue_command(unsigned num_words, const uint32_t *words)
{
	wait_for_space(num_words + 1);

	size_t mask = ring.size() - 1;
	uint64_t wc = write_count.load(std::memory_order_relaxed);
	ring[wc++ & mask] = num_words;
	for (unsigned i = 0; i < num_words; i++)
		ring[wc++ & mask] = words[i];

	publish(wc);
}

void CommandRing::enqueue_commands(const uint32_t *packets, size_t total_words)
{
	size_t mask = ring.size() - 1;
	size_t pos = 0;
	while (pos < total_words)
	{
		uint64_t rc = wait_for_space(packets[pos] + 1);
		uint64_t wc = write_count.load(std::memory_order_relaxed);

		// Copy every packet that fits in the free space, then publish once.
		while (pos < total_words && wc + packets[pos] + 1 <= rc + ring.size())
		{
			size_t packet_words = packets[pos] + 1;
			for (size_t i = 0; i < packet_words; i++)
				ring[wc++ & mask] = packets[pos + i];
			pos += packet_words;
		}

		publish(wc);
	}
}

void CommandRing::thread_loop()
{
	Util::register_thread_index(0);

#ifdef PARALLEL_RDP_SHADER_DIR
	// Here to let the RDP play nice with full Granite.
	// When we move to standalone Granite, we won't need to interact with global subsystems like this.
	Granite::Global::set_thread_context(*global_handles);
	global_handles.reset();
#endif

	// Packets as [num_words, words...]; everything available is taken at once.
	std::vector<uint32_t> tmp_buffer;
	tmp_buffer.reserve(4096);
	size_t mask = ring.size() - 1;
	const uint32_t idle_packet[2] = { 1, uint32_t(Op::MetaIdle) << 24 };

	for (;;)
	{
		const uint64_t rc = read_count.load(std::memory_order_relaxed);
		uint64_t wc = write_count.load();
		bool is_idle = false;

		if (wc == rc)
		{
			std::unique_lock<std::mutex> holder{lock};
			consumer_sleeping = true;
			if (!cond.wait_for(holder, std::chrono::microseconds(500), [&]() { return write_count.load() != rc; }))
				is_idle = true;
			consumer_sleeping = false;
			wc = write_count.load();
		}

		if (is_idle)
		{
			// If we don't receive commands at a steady pace,
			// notify rendering thread that we should probably kick some work.
			tmp_buffer.assign(idle_packet, idle_packet + 2);
		}
		else
		{
			// [rc, wc) is ours until read_count moves past it.
			size_t count = size_t(wc - rc);
			size_t first = std::min(count, ring.size() - size_t(rc & mask));
			tmp_buffer.resize(count);
			memcpy(tmp_buffer.data(), ring.data() + (rc & mask), first * sizeof(uint32_t));
			memcpy(tmp_buffer.data() + first, ring.data(), (count - first) * sizeof(uint32_t));
			read_count.store(wc);
			// Free ring space for a producer waiting on a full ring.
			wake_if(producer_waiting);
		}

		bool exit_requested = false;
		size_t pos = 0;
		while (pos < tmp_buffer.size())
		{
			uint32_t num_words = tmp_buffer[pos];
			// A zero-length packet is the teardown sentinel.
			if (num_words == 0)
			{
				exit_requested = true;
				break;
			}
			processor->enqueue_command_direct(num_words, tmp_buffer.data() + pos + 1);
			pos += num_words + 1;
		}

		if (!is_idle)
		{
			completed_count.store(wc);
			wake_if(drain_waiting);
		}

		if (exit_requested)
			break;
	}
}
}
