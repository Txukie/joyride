/* dirent with d_type, on FindFirstFile (compat_win.c); MinGW's has no d_type */
#ifndef JOYRIDE_DIRENT_H
#define JOYRIDE_DIRENT_H
#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8
struct dirent { unsigned char d_type; char d_name[260]; };
typedef struct joyride_dir DIR;
DIR *joyride_opendir(const char *path);
struct dirent *joyride_readdir(DIR *d);
int joyride_closedir(DIR *d);
#define opendir joyride_opendir
#define readdir joyride_readdir
#define closedir joyride_closedir
#endif
