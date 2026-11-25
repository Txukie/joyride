/* joyride on Windows: the POSIX the runtime uses, on the Win32 API
 * (posix_win.h, sys/mman.h, sys/statvfs.h, dirent.h beside this file). The
 * only file of the runtime that sees windows.h: the runtime itself answers
 * a guest's Win32 under the same names. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/types.h>

#include <stdio.h>
#include <io.h>
#include <fcntl.h>
#include "posix_win.h"
#include "sys/mman.h"
#include "sys/statvfs.h"
#include "dirent.h"

#undef mmap
#undef munmap
#undef mprotect
#undef madvise

/* Files are bytes, as on POSIX: the C runtime's open() and fopen() default
 * to binary, not text (no CRLF translation, no stop at 0x1A); the game's
 * own reads go through them too */
__attribute__((constructor)) static void binary_files(void) { _set_fmode(_O_BINARY); }

/* Sleeps as short as asked: Windows' timer ticks every 15.6 ms unless a
 * program asks for 1 ms, and joyride's waits (the frames' pacing, the
 * frames between steps, the game's timers, handing the machine over) are
 * a millisecond or two; at 15.6 the picture ran at about 20 frames a second */
__attribute__((constructor)) static void fine_timer(void) { timeBeginPeriod(1); }

static DWORD page_prot(int prot) {
    if (!prot) return PAGE_NOACCESS;
    if (prot & PROT_EXEC) return prot & PROT_WRITE ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    return prot & PROT_WRITE ? PAGE_READWRITE : PAGE_READONLY;
}

/* The guest's memory: a range reserved (PROT_NONE: inaccessible, nothing
 * committed), then inside it with MAP_FIXED fresh zero pages (decommitted,
 * committed again: committed pages read as zero) or none (decommitted).
 * Without MAP_FIXED an address is taken exactly or not at all, as
 * MAP_FIXED_NOREPLACE: the guest's 4 GB goes where the lifted code adds. */
void *joyride_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    (void)fd, (void)off;
    if (!len) return MAP_FAILED;
    if (flags & MAP_FIXED) {
        VirtualFree(addr, len, MEM_DECOMMIT);
        if (!prot) return addr;
        return VirtualAlloc(addr, len, MEM_COMMIT, page_prot(prot)) == addr ? addr : MAP_FAILED;
    }
    void *p = VirtualAlloc(addr, len, prot ? MEM_RESERVE | MEM_COMMIT : MEM_RESERVE, prot ? page_prot(prot) : PAGE_NOACCESS);
    if (!p) return MAP_FAILED;
    if (addr && p != addr) { VirtualFree(p, 0, MEM_RELEASE); return MAP_FAILED; }
    return p;
}

int joyride_munmap(void *addr, size_t len) {
    MEMORY_BASIC_INFORMATION mi;
    if (VirtualQuery(addr, &mi, sizeof mi) && mi.AllocationBase == addr)
        return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : -1;
    return VirtualFree(addr, len, MEM_DECOMMIT) ? 0 : -1;
}

int joyride_mprotect(void *addr, size_t len, int prot) {
    DWORD old;
    return VirtualProtect(addr, len, page_prot(prot), &old) ? 0 : -1;
}

int joyride_madvise(void *addr, size_t len, int advice) {
    if (advice != MADV_DONTNEED) return 0;
    VirtualFree(addr, len, MEM_DECOMMIT);            /* zero again: dropped and committed anew */
    return VirtualAlloc(addr, len, MEM_COMMIT, PAGE_READWRITE) ? 0 : -1;
}

long joyride_sysconf(int name) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    if (name == 84) return (long)si.dwNumberOfProcessors;     /* _SC_NPROCESSORS_ONLN */
    if (name == 30) return (long)si.dwPageSize;               /* _SC_PAGESIZE */
    return -1;
}

int joyride_getpagesize(void) { return (int)joyride_sysconf(30); }

static void forward_slashes(char *s) { for (; *s; s++) if (*s == '\\') *s = '/'; }

/* "C:\\x" or "C:/x" -> "/c/x", in place (the buffer has room for one more) */
static void to_posix(char *s) {
    forward_slashes(s);
    if (((s[0] | 0x20) >= 'a' && (s[0] | 0x20) <= 'z') && s[1] == ':') {
        char d = (char)(s[0] | 0x20);
        size_t n = strlen(s + 2);
        if (s[2] == '/') { s[0] = '/', s[1] = d; }               /* "C:/x" -> "/c/x" */
        else { memmove(s + 3, s + 2, n + 1); s[0] = '/', s[1] = d, s[2] = '/'; }   /* "C:" -> "/c/" */
    }
}

const char *joyride_winpath(const char *path) {
    static __thread char buf[2][MAX_PATH * 4];
    static __thread int turn;
    if (!path) return NULL;
    if (path[0] == '/' && ((path[1] | 0x20) >= 'a' && (path[1] | 0x20) <= 'z') && (path[2] == '/' || !path[2])) {
        char *b = buf[turn ^= 1];
        snprintf(b, sizeof buf[0], "%c:/%s", path[1] & ~0x20, path[2] ? path + 3 : "");
        return b;
    }
    return path;
}

/* as realpath: the file must exist; forward slashes */
char *joyride_realpath(const char *path, char *resolved) {
    if (!path) return NULL;
    char buf[MAX_PATH * 4];
    DWORD n = GetFullPathNameA(joyride_winpath(path), sizeof buf, buf, NULL);
    if (!n || n >= sizeof buf || GetFileAttributesA(buf) == INVALID_FILE_ATTRIBUTES) return NULL;
    size_t len = strlen(buf);
    while (len > 3 && (buf[len - 1] == '\\' || buf[len - 1] == '/')) buf[--len] = 0;   /* no trailing slash, as POSIX */
    to_posix(buf);
    len = strlen(buf);
    if (!resolved) return _strdup(buf);
    memcpy(resolved, buf, len + 1);
    return resolved;
}

/* only /proc/self/exe is asked for: this program */
ssize_t joyride_readlink(const char *path, char *buf, size_t n) {
    if (!path || strcmp(path, "/proc/self/exe") || !n) return -1;
    DWORD len = GetModuleFileNameA(NULL, buf, (DWORD)n);
    if (!len || len + 2 >= n) return -1;
    buf[len] = 0;
    to_posix(buf);
    return (ssize_t)strlen(buf);
}

int joyride_statvfs(const char *path, struct statvfs *out) {
    ULARGE_INTEGER avail, total, free_;
    if (!GetDiskFreeSpaceExA(joyride_winpath(path), &avail, &total, &free_)) return -1;
    memset(out, 0, sizeof *out);
    out->f_bsize = out->f_frsize = 4096;
    out->f_blocks = total.QuadPart / 4096, out->f_bfree = free_.QuadPart / 4096, out->f_bavail = avail.QuadPart / 4096;
    return 0;
}

/* ---- files: status and times, as POSIX has them ---- */
#undef stat
#undef fstat
static void from_stat64(const struct _stat64 *s, struct joyride_stat *st) {
    memset(st, 0, sizeof *st);
    st->st_dev = (unsigned long)s->st_dev, st->st_ino = (unsigned long)s->st_ino;
    st->st_mode = s->st_mode, st->st_nlink = s->st_nlink, st->st_size = s->st_size;
    st->st_atim.tv_sec = (time_t)s->st_atime, st->st_mtim.tv_sec = (time_t)s->st_mtime, st->st_ctim.tv_sec = (time_t)s->st_ctime;
}
int joyride_stat(const char *path, struct joyride_stat *st) {
    struct _stat64 s;
    if (_stat64(joyride_winpath(path), &s)) return -1;
    from_stat64(&s, st);
    return 0;
}
int joyride_fstat(int fd, struct joyride_stat *st) {
    struct _stat64 s;
    if (_fstat64(fd, &s)) return -1;
    from_stat64(&s, st);
    return 0;
}
/* access and write times; UTIME_OMIT leaves one as it is */
int joyride_futimens(int fd, const struct timespec ts[2]) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    FILETIME ft[2], *p[2] = { NULL, NULL };
    for (int i = 0; i < 2; i++) {
        if (ts[i].tv_nsec == UTIME_OMIT) continue;
        uint64_t t = ((uint64_t)ts[i].tv_sec + 11644473600ull) * 10000000ull + (uint64_t)ts[i].tv_nsec / 100;
        if (ts[i].tv_nsec == UTIME_NOW) { GetSystemTimeAsFileTime(&ft[i]); p[i] = &ft[i]; continue; }
        ft[i].dwLowDateTime = (DWORD)t, ft[i].dwHighDateTime = (DWORD)(t >> 32), p[i] = &ft[i];
    }
    return SetFileTime(h, NULL, p[0], p[1]) ? 0 : -1;
}
long joyride_tz_offset(void) {                     /* local minus UTC */
    TIME_ZONE_INFORMATION z;
    DWORD r = GetTimeZoneInformation(&z);
    long bias = z.Bias + (r == TIME_ZONE_ID_DAYLIGHT ? z.DaylightBias : r == TIME_ZONE_ID_STANDARD ? z.StandardBias : 0);
    return -bias * 60;
}

/* ---- the file calls, with /c/ paths made C:/ ---- */
#undef open
#undef fopen
#undef mkdir
#undef rmdir
#undef access
#undef unlink
#undef remove
#undef rename
#undef chdir
#undef getcwd
#include <stdarg.h>
int joyride_open(const char *path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    return _open(joyride_winpath(path), flags | O_BINARY, mode);
}
FILE *joyride_fopen(const char *path, const char *mode) { return fopen(joyride_winpath(path), mode); }
int joyride_mkdir(const char *path) { return _mkdir(joyride_winpath(path)); }
int joyride_rmdir(const char *path) { return _rmdir(joyride_winpath(path)); }
int joyride_access(const char *path, int mode) { return _access(joyride_winpath(path), mode & 6); }   /* no X_OK on Windows */
int joyride_unlink(const char *path) { return _unlink(joyride_winpath(path)); }
int joyride_remove(const char *path) { return remove(joyride_winpath(path)); }
int joyride_rename(const char *from, const char *to) {
    char f[MAX_PATH * 4];
    snprintf(f, sizeof f, "%s", joyride_winpath(from));
    return MoveFileExA(f, joyride_winpath(to), MOVEFILE_REPLACE_EXISTING) ? 0 : -1;   /* POSIX rename replaces */
}
int joyride_chdir(const char *path) { return _chdir(joyride_winpath(path)); }
char *joyride_getcwd(char *buf, size_t n) {
    char tmp[MAX_PATH * 4];
    if (!_getcwd(tmp, sizeof tmp)) return NULL;
    to_posix(tmp);
    if (!buf) return _strdup(tmp);
    if (strlen(tmp) + 1 > n) return NULL;
    memcpy(buf, tmp, strlen(tmp) + 1);
    return buf;
}

static int page_readable(const void *p) {
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT) return 0;
    return !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}
int joyride_readable(const void *p, size_t n) {
    return n && page_readable(p) && page_readable((const char *)p + n - 1);
}

struct joyride_dir { HANDLE h; WIN32_FIND_DATAA fd; int first; struct dirent e; };

DIR *joyride_opendir(const char *path) {
    char pat[MAX_PATH * 4];
    snprintf(pat, sizeof pat, "%s/*", joyride_winpath(path));
    DIR *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->h = FindFirstFileA(pat, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) { free(d); return NULL; }
    d->first = 1;
    return d;
}

struct dirent *joyride_readdir(DIR *d) {
    if (!d->first && !FindNextFileA(d->h, &d->fd)) return NULL;
    d->first = 0;
    d->e.d_type = d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? DT_DIR : DT_REG;
    snprintf(d->e.d_name, sizeof d->e.d_name, "%s", d->fd.cFileName);
    return &d->e;
}

int joyride_closedir(DIR *d) {
    if (d) FindClose(d->h), free(d);
    return 0;
}

#endif
