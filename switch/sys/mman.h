#ifndef SWITCH_SYS_MMAN_H
#define SWITCH_SYS_MMAN_H

#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANON 0x20
#define MAP_ANONYMOUS MAP_ANON

#define MAP_FAILED ((void *)-1)

static inline void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset)
{
	(void)addr;
	(void)prot;
	(void)fd;
	(void)offset;

	if ((flags & MAP_ANONYMOUS) == 0)
		return MAP_FAILED;

	size_t size = (len + 0xFFF) & ~((size_t)0xFFF);
	void *ptr = memalign(0x1000, size);
	if (!ptr)
		return MAP_FAILED;

	memset(ptr, 0, size);
	return ptr;
}

static inline int munmap(void *addr, size_t len)
{
	(void)len;
	free(addr);
	return 0;
}

static inline int mprotect(void *addr, size_t len, int prot)
{
	(void)addr;
	(void)len;
	(void)prot;
	return 0;
}

#ifdef __cplusplus
}
#endif

#endif
