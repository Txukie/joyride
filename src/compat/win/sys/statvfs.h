/* statvfs on GetDiskFreeSpaceEx (compat_win.c): the free space a guest asks for */
#ifndef JOYRIDE_SYS_STATVFS_H
#define JOYRIDE_SYS_STATVFS_H
#include <stdint.h>
struct statvfs { unsigned long f_bsize, f_frsize; uint64_t f_blocks, f_bfree, f_bavail; };
int joyride_statvfs(const char *path, struct statvfs *out);
#define statvfs(p, o) joyride_statvfs(p, o)
#endif
