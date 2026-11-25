/* mmap for the runtime's guest memory, on VirtualAlloc (compat_win.c).
 * What it is used for: reserving a range inaccessible (PROT_NONE), and with
 * MAP_FIXED inside one, fresh zero pages (committed) or none (decommitted). */
#ifndef JOYRIDE_SYS_MMAN_H
#define JOYRIDE_SYS_MMAN_H
#include <stddef.h>
#include <sys/types.h>
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_NORESERVE 0x4000
#define MAP_FIXED_NOREPLACE 0x100000
#define MAP_FAILED ((void *)-1)
#define MADV_DONTNEED 4
void *joyride_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
int joyride_munmap(void *addr, size_t len);
int joyride_mprotect(void *addr, size_t len, int prot);
int joyride_madvise(void *addr, size_t len, int advice);
#define mmap joyride_mmap
#define munmap joyride_munmap
#define mprotect joyride_mprotect
#define madvise joyride_madvise
#endif
