/* POSIX functions referenced by libstdc++'s <filesystem> that newlib/PSL1GHT
 * does not provide. lv2 file systems have no symlinks or permission bits. */
#include <errno.h>
#include <sys/types.h>

int symlink(const char* target, const char* linkpath) {
    (void)target;
    (void)linkpath;
    errno = ENOSYS;
    return -1;
}

ssize_t readlink(const char* path, char* buf, size_t size) {
    (void)path;
    (void)buf;
    (void)size;
    errno = EINVAL;
    return -1;
}

long pathconf(const char* path, int name) {
    (void)path;
    (void)name;
    return 1024;
}

int fchmod(int fd, mode_t mode) {
    (void)fd;
    (void)mode;
    return 0;
}
