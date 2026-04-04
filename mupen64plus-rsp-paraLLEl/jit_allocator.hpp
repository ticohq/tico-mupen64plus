#pragma once

#include <vector>
#include <stdint.h>

#if defined(HAVE_LIBNX)
#include <switch.h>
#endif

namespace RSP
{
namespace JIT
{
class Allocator
{
public:
	Allocator() = default;
	~Allocator();
	void operator=(const Allocator &) = delete;
	Allocator(const Allocator &) = delete;

	void *allocate_code(size_t size);
	bool commit_code(void *code, size_t size);
	void *get_executable_code(void *code) const;

private:
	struct Block
	{
		uint8_t *code = nullptr;
#if defined(HAVE_LIBNX)
		uint8_t *rx_code = nullptr;
		Jit jit = {};
#endif
		size_t size = 0;
		size_t offset = 0;
	};
	std::vector<Block> blocks;

	static Block reserve_block(size_t size);
};
}
}
