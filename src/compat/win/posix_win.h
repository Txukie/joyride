/* joyride on Windows: what the runtime (pcrecomp's win32hle, joyride's own)
 * takes from POSIX, for MinGW-w64. Forced into every runtime file; the
 * headers beside it stand in for POSIX ones Windows lacks (sys/mman.h,
 * dirent.h with d_type, sys/statvfs.h). Kept here, not in pcrecomp.
 * windows.h is not included: the runtime has its own Win32 names. */
#ifndef JOYRIDE_POSIX_WIN_H
#define JOYRIDE_POSIX_WIN_H
#ifdef _WIN32

#define _POSIX_THREAD_SAFE_FUNCTIONS 1   /* MinGW's localtime_r, gmtime_r */
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <time.h>
#include <io.h>
#include <direct.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#define mkdir(path, mode) _mkdir(path)
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#define _SC_NPROCESSORS_ONLN 84
#define _SC_PAGESIZE 30
long joyride_sysconf(int name);
#define sysconf joyride_sysconf
int joyride_getpagesize(void);
#define getpagesize joyride_getpagesize
/* the absolute path, with forward slashes (the runtime splits on '/') */
char *joyride_realpath(const char *path, char *resolved);
#define realpath joyride_realpath
ssize_t joyride_readlink(const char *path, char *buf, size_t n);   /* /proc/self/exe: the program */
#define readlink joyride_readlink


/* file status as POSIX has it (nanosecond times: st_mtim, st_atim), on _stat64 */
struct joyride_stat {
    unsigned long st_dev, st_ino;
    unsigned short st_mode;
    short st_nlink;
    long long st_size;
    struct timespec st_atim, st_mtim, st_ctim;
};
int joyride_stat(const char *path, struct joyride_stat *st);
int joyride_fstat(int fd, struct joyride_stat *st);
#define stat joyride_stat
#define fstat joyride_fstat
#define lstat joyride_stat
#define UTIME_NOW  ((1l << 30) - 1l)
#define UTIME_OMIT ((1l << 30) - 2l)
int joyride_futimens(int fd, const struct timespec ts[2]);
#define futimens joyride_futimens
#define timegm _mkgmtime
/* the local time's offset from UTC, seconds: read in one place,
 * `t.tm_gmtoff`, which this turns into a call (struct tm has no such field) */
long joyride_tz_offset(void);
#define tm_gmtoff tm_isdst * 0 + joyride_tz_offset()

/* Paths. The runtime's path logic is POSIX's: absolute is "/...". So the
 * runtime sees Windows paths as MSYS writes them, /c/users/..., and they
 * become C:/users/... again at the file calls below (and whatever else takes
 * a path: joyride_winpath). realpath, getcwd and readlink give the former. */
const char *joyride_winpath(const char *path);           /* "/c/x" -> "C:/x" (a buffer per thread, two in turn) */
#define JOYRIDE_OSPATH(p) joyride_winpath(p)                /* a path for a library (SDL, ffmpeg) */
int joyride_open(const char *path, int flags, ...);
FILE *joyride_fopen(const char *path, const char *mode);
int joyride_mkdir(const char *path);
int joyride_rmdir(const char *path);
int joyride_access(const char *path, int mode);
int joyride_unlink(const char *path);
int joyride_remove(const char *path);
int joyride_rename(const char *from, const char *to);
int joyride_chdir(const char *path);
char *joyride_getcwd(char *buf, size_t n);
#undef mkdir
#define mkdir(path, mode) joyride_mkdir(path)
#define open joyride_open
#define fopen joyride_fopen
#define rmdir joyride_rmdir
#define access joyride_access
#define unlink joyride_unlink
#define remove joyride_remove
#define rename joyride_rename
#define chdir joyride_chdir
#define getcwd joyride_getcwd

/* memory the game may have freed: readable now (VirtualQuery) */
int joyride_readable(const void *p, size_t n);

#endif
#endif
