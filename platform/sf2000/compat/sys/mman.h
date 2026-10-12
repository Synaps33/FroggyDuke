/*
 * sys/mman.h for the SF2000 / GB300.
 *
 * The console's newlib has no memory-mapping API at all -- there is no MMU and
 * no demand-paged filesystem -- but mio.hpp includes <sys/mman.h> on anything
 * that is not _WIN32 and then defines an inline mmap source around it.
 *
 * eduke32 never uses that source: GRP and RTS files are read through stdio.
 * So this header exists only to let mio.hpp compile. The declarations are real
 * declarations with no implementation behind them on the console, which is the
 * behaviour we want: if anything ever does call mmap, the link fails loudly
 * instead of silently returning a bogus pointer.
 */
#ifndef SF2K_SYS_MMAN_H
#define SF2K_SYS_MMAN_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_SHARED    0x0001
#define MAP_PRIVATE   0x0002
#define MAP_FIXED     0x0010
#define MAP_ANONYMOUS 0x0020
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_FAILED    ((void *) -1)

#define MS_ASYNC     1
#define MS_INVALIDATE 2
#define MS_SYNC      4

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset);
int   munmap(void *addr, size_t len);
int   mprotect(void *addr, size_t len, int prot);
int   madvise(void *addr, size_t len, int advice);
int   msync(void *addr, size_t len, int flags);

#ifdef __cplusplus
}
#endif

#endif /* SF2K_SYS_MMAN_H */