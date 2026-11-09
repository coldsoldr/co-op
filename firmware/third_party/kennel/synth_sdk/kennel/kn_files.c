/*
 * Files under /yaffs2/user/ for features (op1_feature.h op1_file_*), e.g. tape undo's history: R246's YAFFS calls with
 * the path checked and only handles this API opened accepted (a feature never touches the tape file's or the
 * databases' descriptors). Main loop.
 *
 * R246 / YAFFS2 Direct: yaffs_open 0x01A997F4 (path, flags, mode; flags as Linux: 0 read, 1 write, 2 both, 0x40
 * create, 0x200 truncate; R246 creates with 0x241, mode 0x180), yaffs_close 0x01A99908, yaffs_read 0x01A99C90,
 * yaffs_write 0x01A99F84, yaffs_lseek 0x01A9A0E4 (whence 0 / 1 / 2), yaffs_unlink 0x01A9A3FC (the delete
 * tape_image_recreate and firmware_update_unpack use), yaffs_mkdir 0x01A9A886.
 * Copyright (C) 2026 Hundekraft. GPL-3.0-or-later with the OP-1 firmware permission (see LICENSE and NOTICE).
 */
#pragma GCC optimize("no-tree-loop-distribute-patterns")          /* the firmware has no memcpy / memset */
#include "../op1_feature.h"

#ifdef OP1_HOST                                         /* harness/files_test.c */
int host_y_open(const char *p, int flags, int mode), host_y_close(int fd), host_y_read(int fd, void *d, int n);
int host_y_write(int fd, const void *s, int n), host_y_unlink(const char *p), host_y_mkdir(const char *p, int mode);
int32_t host_y_lseek(int fd, int32_t off, int whence);
#define Y_OPEN    host_y_open
#define Y_CLOSE   host_y_close
#define Y_READ    host_y_read
#define Y_WRITE   host_y_write
#define Y_LSEEK   host_y_lseek
#define Y_UNLINK  host_y_unlink
#define Y_MKDIR   host_y_mkdir
#else
#define Y_OPEN    ((int (*)(const char *, int, int))0x01A997F4)
#define Y_CLOSE   ((int (*)(int))0x01A99908)
#define Y_READ    ((int (*)(int, void *, int))0x01A99C90)
#define Y_WRITE   ((int (*)(int, const void *, int))0x01A99F84)
#define Y_LSEEK   ((int32_t (*)(int, int32_t, int))0x01A9A0E4)
#define Y_UNLINK  ((int (*)(const char *))0x01A9A3FC)
#define Y_MKDIR   ((int (*)(const char *, int))0x01A9A886)
#endif
#define MAX_OPEN  8

static int open_fd[MAX_OPEN] = { -1, -1, -1, -1, -1, -1, -1, -1 };

/* /yaffs2/user/ then [A-Za-z0-9._ -] names, single slashes, no "." or ".." part, under 120 characters */
static int path_ok(const char *p)
{
    static const char root[] = "/yaffs2/user/";
    int i = 0;
    for (; root[i]; i++) if (p[i] != root[i]) return 0;
    int start = i;
    for (; p[i]; i++) {
        char c = p[i];
        if (i >= 120) return 0;
        if (c == '/') {
            int n = i - start;
            if (!n || (n == 1 && p[start] == '.') || (n == 2 && p[start] == '.' && p[start + 1] == '.')) return 0;
            start = i + 1;
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '-' || c == ' ')) return 0;
    }
    int n = i - start;                                  /* the last part: a name */
    return n && !(n == 1 && p[start] == '.') && !(n == 2 && p[start] == '.' && p[start + 1] == '.');
}
static int slot_of(int fd)
{
    if (fd < 0) return -1;
    for (int i = 0; i < MAX_OPEN; i++) if (open_fd[i] == fd) return i;
    return -1;
}

int op1_file_open(const char *path, int flags)
{
    if (!path || !path_ok(path) || (flags & ~(OP1_FILE_ACCESS | OP1_FILE_CREATE | OP1_FILE_TRUNC)) ||
        (flags & OP1_FILE_ACCESS) == OP1_FILE_ACCESS) return -1;
    int s = -1;
    for (int i = 0; i < MAX_OPEN && s < 0; i++) if (open_fd[i] < 0) s = i;
    if (s < 0) return -1;                               /* MAX_OPEN open already */
    int fd = Y_OPEN(path, flags, 0x180);
    if (fd < 0) return -1;
    open_fd[s] = fd;
    return fd;
}
int op1_file_close(int fd)
{
    int s = slot_of(fd);
    if (s < 0) return -1;
    open_fd[s] = -1;
    return Y_CLOSE(fd);
}
int op1_file_read(int fd, void *dst, int len) { return slot_of(fd) < 0 || len < 0 || !dst ? -1 : Y_READ(fd, dst, len); }
int op1_file_write(int fd, const void *src, int len) { return slot_of(fd) < 0 || len < 0 || !src ? -1 : Y_WRITE(fd, src, len); }
int32_t op1_file_seek(int fd, int32_t off, int whence)
{
    return slot_of(fd) < 0 || whence < 0 || whence > 2 ? -1 : Y_LSEEK(fd, off, whence);
}
int op1_file_unlink(const char *path) { return path && path_ok(path) ? Y_UNLINK(path) : -1; }
int op1_file_mkdir(const char *path) { return path && path_ok(path) ? Y_MKDIR(path, 0) : -1; }
