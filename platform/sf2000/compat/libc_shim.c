/* Bare-metal C library gaps for the SF2000/GB300 build.
 *
 * The Frog newlib provides malloc/free and the basics, but not the full POSIX
 * surface that upstream eduke32 and its bundled libraries reach for. Nothing
 * here emulates a real operating system: the console runs one game thread
 * driven by the frontend's frame callback, so "atomic" and "signal" handling
 * only have to be correct and cheap, not concurrent.
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ atomics
 *
 * The console's libc has no libatomic, but GCC lowers 8-byte accesses on a
 * 32-bit target into __atomic_* libcalls. Provide them on top of plain
 * accesses bracketed by __sync_synchronize(), which MIPS lowers to `sync`.
 * The game logic is single-threaded (the coroutine and the audio pump are
 * driven from the same frame callback), so disabling interrupts around the
 * read-modify-write is enough to keep the operation indivisible.
 *
 * Sizes 1/2/4 are normally emitted inline by -march=mips32; they are defined
 * here as well so that a toolchain which decides to call out for them still
 * links.
 */

#define EDUKE_ATOMIC_SHIM(size, ctype)                                                        \
    ctype __atomic_load_##size(const volatile void *ptr, int memorder)                        \
    {                                                                                         \
        (void)memorder;                                                                       \
        ctype v = *(const volatile ctype *)ptr;                                               \
        __sync_synchronize();                                                                 \
        return v;                                                                             \
    }                                                                                         \
                                                                                              \
    void __atomic_store_##size(volatile void *ptr, ctype val, int memorder)                  \
    {                                                                                         \
        (void)memorder;                                                                       \
        __sync_synchronize();                                                                 \
        *(volatile ctype *)ptr = val;                                                         \
        __sync_synchronize();                                                                 \
    }                                                                                         \
                                                                                              \
    ctype __atomic_exchange_##size(volatile void *ptr, ctype val, int memorder)               \
    {                                                                                         \
        (void)memorder;                                                                       \
        __sync_synchronize();                                                                 \
        ctype old = *(volatile ctype *)ptr;                                                   \
        *(volatile ctype *)ptr = val;                                                         \
        __sync_synchronize();                                                                 \
        return old;                                                                           \
    }                                                                                         \
                                                                                              \
    char __atomic_compare_exchange_##size(volatile void *ptr, void *expected, void *desired, \
                                          char weak, int success, int failure)               \
    {                                                                                         \
        (void)weak;                                                                           \
        (void)success;                                                                        \
        (void)failure;                                                                        \
        ctype *exp = (ctype *)expected;                                                       \
        __sync_synchronize();                                                                 \
        if (*(volatile ctype *)ptr == *exp) {                                                 \
            *(volatile ctype *)ptr = *(const volatile ctype *)desired;                        \
            __sync_synchronize();                                                             \
            return 1;                                                                         \
        }                                                                                     \
        *exp = *(volatile ctype *)ptr;                                                        \
        return 0;                                                                             \
    }

EDUKE_ATOMIC_SHIM(1, char)
EDUKE_ATOMIC_SHIM(2, short)
EDUKE_ATOMIC_SHIM(4, int)
EDUKE_ATOMIC_SHIM(8, long long)

/* Fetch-and-modify is only ever requested for the 4- and 8-byte forms. The
 * operation is applied through a macro so that C++'s alternative spellings
 * (and/or/xor) never have to appear as tokens in this C translation unit. */
#define EDUKE_OP_add(a, b) ((a) + (b))
#define EDUKE_OP_sub(a, b) ((a) - (b))
#define EDUKE_OP_and(a, b) ((a) & (b))
#define EDUKE_OP_or(a, b) ((a) | (b))
#define EDUKE_OP_xor(a, b) ((a) ^ (b))

#define EDUKE_ATOMIC_FETCHOP(name, op, size, ctype)                                          \
    ctype __atomic_fetch_##name##_##size(volatile void *ptr, ctype val, int memorder)        \
    {                                                                                         \
        (void)memorder;                                                                       \
        __sync_synchronize();                                                                 \
        ctype old = *(volatile ctype *)ptr;                                                   \
        *(volatile ctype *)ptr = op(old, val);                                                \
        __sync_synchronize();                                                                 \
        return old;                                                                           \
    }

EDUKE_ATOMIC_FETCHOP(add, EDUKE_OP_add, 4, int)
EDUKE_ATOMIC_FETCHOP(sub, EDUKE_OP_sub, 4, int)
EDUKE_ATOMIC_FETCHOP(and, EDUKE_OP_and, 4, int)
EDUKE_ATOMIC_FETCHOP(or, EDUKE_OP_or, 4, int)
EDUKE_ATOMIC_FETCHOP(xor, EDUKE_OP_xor, 4, int)
EDUKE_ATOMIC_FETCHOP(add, EDUKE_OP_add, 8, long long)
EDUKE_ATOMIC_FETCHOP(sub, EDUKE_OP_sub, 8, long long)
EDUKE_ATOMIC_FETCHOP(and, EDUKE_OP_and, 8, long long)
EDUKE_ATOMIC_FETCHOP(or, EDUKE_OP_or, 8, long long)
EDUKE_ATOMIC_FETCHOP(xor, EDUKE_OP_xor, 8, long long)

/* ------------------------------------------------------- aligned allocation
 *
 * libasync's aligned_alloc() uses posix_memalign directly. newlib's malloc
 * returns suitably aligned blocks for any fundamental type, so over-allocate
 * and hand back an aligned interior pointer, keeping the original just below
 * it for aligned_free().
 */

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
    size_t align;
    size_t slack;
    char *base;
    char *aligned;

    if (memptr == NULL)
        return 22 /* EINVAL */;

    /* POSIX requires a power-of-two alignment at least as large as a pointer.
     * Rounding up to the fundamental-type alignment is safe for every caller
     * here and keeps newlib's malloc from being asked for odd alignments. */
    align = alignment < sizeof(long long) ? sizeof(long long) : alignment;
    if ((align & (align - 1)) != 0)
        return 22 /* EINVAL */;

    /* Room for the alignment padding plus the saved base pointer. */
    slack = align + sizeof(void *);

    base = (char *)malloc(size ? size + slack : slack);
    if (base == NULL)
        return 12 /* ENOMEM */;

    aligned = base + sizeof(void *);
    aligned += (align - ((size_t)aligned & (align - 1))) & (align - 1);

    ((void **)aligned)[-1] = base;
    *memptr               = aligned;
    return 0;
}

/* --------------------------------------------------------------- signals
 *
 * loguru installs fatal-signal handlers through sigaction() and ignores the
 * return value. The console runtime has no signals to catch, so this is a
 * successful no-op rather than an error.
 */

struct sigaction;
struct sigset_t;

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{
    (void)signum;
    (void)act;
    (void)oldact;
    return 0;
}