/// @file TicoAudio.h
/// @brief Audio for the tico frontend
/// Uses callback-based audio with ring buffer and resampling

#pragma once

#include <SDL.h>
#include <SDL_mixer.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>
#ifdef __SWITCH__
#include <switch.h>
extern "C" {
enum rsp_plugin_type
{
    RSP_PLUGIN_NONE = 0,
    RSP_PLUGIN_HLE,
    RSP_PLUGIN_CXD4,
    RSP_PLUGIN_PARALLEL,
    RSP_PLUGIN_MAX
};

extern enum rsp_plugin_type current_rsp_type;
}
#endif
#include "TicoConfig.h"
#include "TicoLogger.h"

/// @brief Thread-safe lock-free SPSC ring buffer for audio samples
template <typename T>
class TicoRingBuffer
{
public:
    explicit TicoRingBuffer(size_t size) : m_buffer(size), m_head(0), m_tail(0) {}

    void Write(const T *data, size_t count)
    {
        size_t currentTail = m_tail.load(std::memory_order_relaxed);
        size_t currentHead = m_head.load(std::memory_order_acquire);
        size_t capacity = m_buffer.size();

        size_t freeSpace = (capacity + currentHead - currentTail - 1) % capacity;
        size_t toWrite = (count < freeSpace) ? count : freeSpace;

        if (toWrite == 0)
            return;

        size_t part1 = std::min(toWrite, capacity - currentTail);
        std::copy(data, data + part1, m_buffer.begin() + currentTail);

        if (part1 < toWrite)
        {
            std::copy(data + part1, data + toWrite, m_buffer.begin());
        }

        m_tail.store((currentTail + toWrite) % capacity, std::memory_order_release);
    }

    size_t Read(T *data, size_t count)
    {
        size_t currentHead = m_head.load(std::memory_order_relaxed);
        size_t currentTail = m_tail.load(std::memory_order_acquire);
        size_t capacity = m_buffer.size();

        size_t available = (capacity + currentTail - currentHead) % capacity;
        size_t toRead = (count < available) ? count : available;

        if (toRead == 0)
            return 0;

        size_t part1 = std::min(toRead, capacity - currentHead);
        std::copy(m_buffer.begin() + currentHead,
                  m_buffer.begin() + currentHead + part1, data);

        if (part1 < toRead)
        {
            size_t part2 = toRead - part1;
            std::copy(m_buffer.begin(), m_buffer.begin() + part2, data + part1);
        }

        m_head.store((currentHead + toRead) % capacity, std::memory_order_release);
        return toRead;
    }

    size_t Available() const
    {
        size_t currentHead = m_head.load(std::memory_order_relaxed);
        size_t currentTail = m_tail.load(std::memory_order_relaxed);
        size_t capacity = m_buffer.size();
        return (capacity + currentTail - currentHead) % capacity;
    }

    size_t GetAvailableWrite() const
    {
        size_t currentHead = m_head.load(std::memory_order_relaxed);
        size_t currentTail = m_tail.load(std::memory_order_relaxed);
        size_t capacity = m_buffer.size();
        size_t used = (capacity + currentTail - currentHead) % capacity;
        return (capacity - 1) - used;
    }

    void Clear()
    {
        m_head.store(0, std::memory_order_release);
        m_tail.store(0, std::memory_order_release);
    }

private:
    std::vector<T> m_buffer;
    std::atomic<size_t> m_head;
    std::atomic<size_t> m_tail;
};

/// @brief Audio manager: the game's samples to SDL_mixer
/// @details Uses Mix_HookMusic callback with ring buffer and optional resampling
class TicoAudio
{
public:
    static constexpr int SAMPLE_RATE = 44100;
    static constexpr int CHANNELS = 2;
    static constexpr size_t BUFFER_SIZE = SAMPLE_RATE * 6;
    
    // Latency cap. 80ms: must hold the prime level (~35ms) plus one consumer period
    // (~23ms) plus one video frame of production (~17ms) without tripping the producer
    // throttle; the old 50ms cap left the buffer operating below one consumer period,
    // which underruns (and stutters) on every callback.
    static constexpr size_t MAX_BUFFERED_SAMPLES = (SAMPLE_RATE * 80 / 1000) * CHANNELS; // 80ms (7056 samples)
    // Silence pre-fill written when the ring runs dry: 3072 int16 ≈ 35ms of stereo,
    // 1.5x the consumer's 2048-sample callback period.
    static constexpr size_t PRIME_SAMPLES = 3072;
    static constexpr size_t SDL_QUEUE_MAX_BYTES = MAX_BUFFERED_SAMPLES * sizeof(int16_t);

    // Hard cap so audio backpressure can NEVER block the emulation thread forever.
    // ~50ms on Switch (100µs/spin). If the sink won't drain within this window (e.g. a
    // stalled audout consumer), drop samples instead of spinning — a brief audio glitch
    // beats a hard freeze that needs a force-exit. This is the emulation thread, so an
    // unbounded wait here deadlocks retro_run().
    static constexpr int MAX_BACKPRESSURE_SPINS = 500;

    TicoAudio() : m_buffer(BUFFER_SIZE), m_resampler(nullptr), m_deviceId(0),
                  m_initialized(false), m_paused(false), m_coreSampleRate(SAMPLE_RATE) {}

    // Bounded wait for room in the audio sink. Returns true if there is room to write,
    // false if the sink is full and the caller should drop the samples.
    //
    // Emulation timing is governed by vsync (FIFO present) + the frame pacer, NOT by this
    // throttle, so it must never govern frame rate. On the parallel-RDP path the whole
    // emulator runs on one cooperative thread and the SDL_mixer callback gets starved, so
    // the buffer never drains; spinning the full cap on every push there crushed the
    // framerate. Hysteresis fixes it: once we see the sink isn't draining we mark it
    // "stalled" and drop immediately (no spin) until it recovers below half-full. On the
    // GLideN64 path the sink drains normally, so this never trips and behaviour is
    // unchanged.
    bool WaitForAudioRoom(bool useQueue)
    {
        if (m_paused)
            return true;

        const size_t maxLevel = useQueue ? SDL_QUEUE_MAX_BYTES : MAX_BUFFERED_SAMPLES;
        auto level = [&]() -> size_t {
            return useQueue ? (size_t)SDL_GetQueuedAudioSize(m_deviceId)
                            : m_buffer.Available();
        };

        if (m_audioSinkStalled)
        {
            if (level() < maxLevel / 2)
                m_audioSinkStalled = false; // drained with margin — resume normal throttling
            else
            {
                NoteAudioStall();
                // Brief yield so the SDL audio consumer thread gets a scheduling slice.
                // On the parallel-RDP path the consumer may be stuck sharing the pegged
                // emulation core until its first callback runs and pins itself to core 0;
                // this hands it that first slice to bootstrap. Bounded + tiny so it can't
                // tank the framerate the way the old unbounded spin did. Once the consumer
                // reaches core 0 it drains on its own and we stop hitting this path.
#ifdef __SWITCH__
                svcSleepThread(500000); // 500µs
#endif
                return false;
            }
        }

        int spins = 0;
        while (level() >= maxLevel)
        {
            if (m_paused)
                return true;
            if (++spins > MAX_BACKPRESSURE_SPINS)
            {
                m_audioSinkStalled = true;
                NoteAudioStall();
                return false;
            }
#ifdef __SWITCH__
            svcSleepThread(100000); // 100µs yield
#else
            SDL_Delay(1);
#endif
        }
        return true;
    }

    // Diagnostics for the main-loop heartbeat (the AUDIO log category is disabled in
    // release builds, so audio state must be reported via a visible category).
    uint64_t GetConsumerCalls() const { return m_consumerCalls.load(std::memory_order_relaxed); }
    uint64_t GetStallCount() const { return m_audioStallCount; }
    size_t GetBufferedSamples() { return m_buffer.Available(); }
    bool IsSinkStalled() const { return m_audioSinkStalled; }
    uint32_t GetUnderrunCount() const { return m_underrunCount.load(std::memory_order_relaxed); }
    uint64_t GetPrimeCount() const { return m_primeCount; }

    // When the ring runs dry, pre-fill silence before writing real samples. The consumer
    // takes 2048 int16 per callback while the producer adds ~1470 per video frame; if the
    // mean fill level sits below one consumer period, the sawtooth bottoms out on every
    // callback and each one pads with zeros — constant crackle. One deliberate ~35ms
    // insertion moves the operating point up so the sawtooth floats clear of empty; it
    // only recurs if clock drift drains the ring again (rare single hiccup instead).
    // Producer-thread only, preserving the ring's single-producer/single-consumer model.
    void PrimeIfEmpty()
    {
        if (m_buffer.Available() != 0)
            return;
        int16_t zeros[512] = {};
        size_t remaining = PRIME_SAMPLES;
        while (remaining > 0)
        {
            size_t n = std::min(remaining, sizeof(zeros) / sizeof(zeros[0]));
            m_buffer.Write(zeros, n);
            remaining -= n;
        }
        m_primeCount++;
    }

    void NoteAudioStall()
    {
        if ((m_audioStallCount++ % 240) == 0)
            LOG_WARN("CORE", "AUDIO: sink not draining (stall #%llu, consumer_calls=%llu) — dropping samples",
                     (unsigned long long)m_audioStallCount,
                     (unsigned long long)m_consumerCalls.load(std::memory_order_relaxed));
    }

    ~TicoAudio()
    {
        Shutdown();
    }

    /// Initialize audio system
    bool Init(SDL_AudioDeviceID deviceId = 0)
    {
        if (m_initialized)
            return true;

        if (TicoConfig::USE_SDLQUEUEAUDIO)
        {
            if (deviceId == 0)
            {
                LOG_ERROR("AUDIO", "SDL_QueueAudio mode requires a valid device ID");
                return false;
            }
            m_deviceId = deviceId;
            SDL_PauseAudioDevice(m_deviceId, 0);
            LOG_AUDIO("Initialized with SDL_QueueAudio (Push)");
        }
        else
        {
            Mix_HookMusic(AudioCallback, this);
            LOG_AUDIO("Initialized with callback-based audio");
        }

        m_initialized = true;
        return true;
    }

    /// Shutdown audio system
    void Shutdown()
    {
        if (!m_initialized)
            return;

        if (TicoConfig::USE_SDLQUEUEAUDIO)
        {
            SDL_PauseAudioDevice(m_deviceId, 1);
            SDL_CloseAudioDevice(m_deviceId);
            m_deviceId = 0;
        }
        else
        {
            Mix_HookMusic(nullptr, nullptr);
        }

        if (m_resampler)
        {
            SDL_FreeAudioStream(m_resampler);
            m_resampler = nullptr;
        }

        m_initialized = false;
        LOG_AUDIO("Shutdown complete");
    }

    /// Set the core's sample rate for resampling
    void SetCoreSampleRate(double sampleRate)
    {
        if (sampleRate <= 0)
            sampleRate = SAMPLE_RATE;

        int coreSR = static_cast<int>(sampleRate);
        if (coreSR == m_coreSampleRate && m_resampler != nullptr)
        {
            return;
        }

        m_coreSampleRate = coreSR;

        // the game changes rate from the emulation thread while SDL's
        // callback reads the stream
        SDL_AudioStream *next = coreSR != SAMPLE_RATE
            ? SDL_NewAudioStream(AUDIO_S16, CHANNELS, coreSR, AUDIO_S16, CHANNELS, SAMPLE_RATE)
            : nullptr;
        SDL_LockAudio();
        SDL_AudioStream *previous = m_resampler;
        m_resampler = next;
        SDL_UnlockAudio();
        if (previous)
            SDL_FreeAudioStream(previous);
        if (next)
            LOG_AUDIO("Resampler created: %d -> %d Hz", coreSR, SAMPLE_RATE);
    }

    /// Push a single audio sample (left, right)
    void PushSample(int16_t left, int16_t right)
    {
        if (!m_initialized)
            return;
        if (m_fastForward)
            return; // fast-forwarded audio is dropped, not sped up

        if (TicoConfig::USE_SDLQUEUEAUDIO)
        {
            if (!WaitForAudioRoom(/*useQueue=*/true))
                return; // sink stalled — drop rather than freeze the emulator

            int16_t samples[2] = {left, right};
            SDL_QueueAudio(m_deviceId, samples, sizeof(samples));
        }
        else
        {
            if (!WaitForAudioRoom(/*useQueue=*/false))
                return;

            PrimeIfEmpty();
            int16_t samples[2] = {left, right};
            m_buffer.Write(samples, 2);
        }
    }

    /// Push a batch of audio samples
    size_t PushSamples(const int16_t *data, size_t frames)
    {
        if (!m_initialized || !data || frames == 0)
            return 0;
        if (m_fastForward)
            return frames;

        size_t samplesNeeded = frames * CHANNELS;

        if (TicoConfig::USE_SDLQUEUEAUDIO)
        {
            if (!WaitForAudioRoom(/*useQueue=*/true))
                return frames; // sink stalled — drop batch (report consumed) instead of freezing

            SDL_QueueAudio(m_deviceId, data, samplesNeeded * sizeof(int16_t));
        }
        else
        {
            if (!WaitForAudioRoom(/*useQueue=*/false))
                return frames;

            PrimeIfEmpty();
            m_buffer.Write(data, samplesNeeded);
        }
        return frames;
    }

    /// Flush/clear the audio buffer
    void Flush()
    {
        if (TicoConfig::USE_SDLQUEUEAUDIO)
        {
            SDL_ClearQueuedAudio(m_deviceId);
        }
        else
        {
            m_buffer.Clear();
            if (m_resampler)
            {
                SDL_AudioStreamClear(m_resampler);
            }
        }
        LOG_AUDIO("Buffer flushed");
    }

    /// Pause/unpause
    void SetPaused(bool paused) { m_paused = paused; }
    bool IsPaused() const { return m_paused; }

    /// Enable/disable fast forward
    void SetFastForward(bool ff) { m_fastForward = ff; }
    bool IsFastForwarding() const { return m_fastForward; }

private:
    /// @brief SDL_mixer pull callback — reads from ring buffer with optional resampling
    static void AudioCallback(void *userdata, uint8_t *stream, int len)
    {
        TicoAudio *self = static_cast<TicoAudio *>(userdata);
        if (!self)
        {
            memset(stream, 0, len);
            return;
        }

        self->m_consumerCalls.fetch_add(1, std::memory_order_relaxed);

#ifdef __SWITCH__
        // Re-evaluate the pin on every callback instead of latching the first result:
        // callbacks start at Mix_OpenAudio, long before the core sets current_rsp_type,
        // so a pin-once lands on core 1 with rsp still NONE. On the parallel-RDP path
        // the emulator then pegs core 1 (the libco coroutine re-pins the main thread
        // from core 2 to core 1) and HOS strict-priority scheduling starves this thread
        // to literally zero CPU — audio goes silent. Core 2 is the free core in that
        // mode; GLideN64-threaded is the mirror image (emu core 0, render core 2), so
        // it keeps core 1. The re-pin costs one compare per callback; the syscall only
        // fires when the preferred core actually changes.
        static thread_local int s_audioPinnedCore = -1;
        const int preferredCore = (current_rsp_type == RSP_PLUGIN_PARALLEL) ? 2 : 1;
        if (s_audioPinnedCore != preferredCore)
        {
            Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, preferredCore, 1u << preferredCore);
            if (R_SUCCEEDED(rc))
                s_audioPinnedCore = preferredCore;
            LOG_WARN("CORE", "AUDIO: callback thread pinned to core %d (rc=0x%x)", preferredCore, rc);
        }
#endif

        memset(stream, 0, len);

        size_t bytesRead = 0;
        const size_t requestedSamples = static_cast<size_t>(len) / sizeof(int16_t);

        if (self->m_resampler)
        {
            int availableBytes = SDL_AudioStreamAvailable(self->m_resampler);

            while (availableBytes < len)
            {
                int16_t tempBuf[4096];
                size_t bufferedSamples = self->m_buffer.Available();
                size_t toRead = std::min(bufferedSamples, sizeof(tempBuf) / sizeof(tempBuf[0]));
                if (toRead == 0)
                    break;

                size_t read = self->m_buffer.Read(tempBuf, toRead);
                if (read == 0)
                    break;

                SDL_AudioStreamPut(self->m_resampler, tempBuf, read * sizeof(int16_t));
                availableBytes = SDL_AudioStreamAvailable(self->m_resampler);
            }

            int resampled = SDL_AudioStreamGet(self->m_resampler, stream, len);
            if (resampled > 0)
                bytesRead = static_cast<size_t>(resampled);
        }
        else
        {
            size_t bufferedSamples = self->m_buffer.Available();
            size_t toRead = std::min(bufferedSamples, requestedSamples);
            size_t read = self->m_buffer.Read(reinterpret_cast<int16_t *>(stream), toRead);
            bytesRead = read * sizeof(int16_t);
        }

        if (bytesRead < static_cast<size_t>(len) && !self->m_paused)
        {
            uint32_t underrunCount = self->m_underrunCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((underrunCount % 120) == 1)
            {
                LOG_WARN("AUDIO", "Audio underrun #%u (requested=%d bytes, provided=%zu bytes, buffered=%zu samples)",
                         underrunCount, len, bytesRead, self->m_buffer.Available());
            }
        }
    }

    TicoRingBuffer<int16_t> m_buffer;
    SDL_AudioStream *m_resampler;
    SDL_AudioDeviceID m_deviceId;
    bool m_initialized;
    bool m_paused;
    std::atomic<bool> m_fastForward{false};
    int m_coreSampleRate;
    std::atomic<uint32_t> m_underrunCount{0};
    uint64_t m_audioStallCount = 0; // emulation-thread only; counts backpressure drops
    bool m_audioSinkStalled = false; // sticky: sink isn't draining, drop instead of spinning
    std::atomic<uint64_t> m_consumerCalls{0}; // incremented by the SDL audio callback thread
    uint64_t m_primeCount = 0; // producer-thread only; silence pre-fills after the ring ran dry
};
