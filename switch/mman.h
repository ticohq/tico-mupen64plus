/// @file mman.h
/// @brief POSIX mmap/mprotect/munmap shims for Nintendo Switch
/// @details Routes JIT allocations through libnx Jit API (RW/RX dual-mapping)
///          and MAP_ANONYMOUS requests through memalign.
#ifndef MMAN_H
#define MMAN_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <malloc.h>
#include <switch.h>

#define PROT_READ 0b001
#define PROT_WRITE 0b010
#define PROT_EXEC 0b100
#define MAP_PRIVATE 2
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20

#define MAP_FAILED ((void *)-1)

Jit mupen_jit;
bool mupen_jit_active = false;
void* mupen_jit_rx_addr = NULL;

/// @brief Allocate memory with optional JIT dual-mapping
/// @details MAP_ANONYMOUS → memalign, otherwise → libnx Jit (RW/RX pair)
static inline void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset)
{
    (void)fd;
    (void)offset;

    size_t size = (len + 0xFFF) &~ 0xFFF;

    if (flags & MAP_ANONYMOUS) {
        void *ptr = memalign(0x1000, size);
        if (ptr) memset(ptr, 0, size);
        return ptr ? ptr : MAP_FAILED;
    }

    if (R_SUCCEEDED(jitCreate(&mupen_jit, size)))
    {
        if (R_SUCCEEDED(jitTransitionToWritable(&mupen_jit)))
        {
            mupen_jit_active = true;
            mupen_jit_rx_addr = jitGetRxAddr(&mupen_jit);
            return jitGetRwAddr(&mupen_jit);
        }
        jitClose(&mupen_jit);
    }

    printf("[NXJIT]: Jit failed!\n");
    return MAP_FAILED;
}

/// @brief Transition JIT buffer between writable and executable states
static inline int mprotect(void *addr, size_t len, int prot)
{
    if (!mupen_jit_active) return 0;

    if (prot & PROT_EXEC)
        jitTransitionToExecutable(&mupen_jit);
    else if ((prot & PROT_WRITE) || (prot & PROT_READ))
        jitTransitionToWritable(&mupen_jit);

    return 0;
}

/// @brief Free JIT or memalign'd memory
static inline int munmap(void *addr, size_t len)
{
    if (mupen_jit_active)
    {
        void* ptr_rw = jitGetRwAddr(&mupen_jit);
        if (addr == ptr_rw || addr == mupen_jit_rx_addr)
        {
            jitClose(&mupen_jit);
            mupen_jit_active = false;
            mupen_jit_rx_addr = NULL;
            return 0;
        }
    }

    if (addr && addr != MAP_FAILED)
        free(addr);

    return 0;
}

#ifdef __cplusplus
};
#endif

#endif // MMAN_H
