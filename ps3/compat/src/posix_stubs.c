/* POSIX functions referenced by libstdc++'s <filesystem> that newlib/PSL1GHT
 * does not provide. lv2 file systems have no symlinks or permission bits. */
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

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

/* lv2 has no descriptor duplication or hard links. */
int dup(int fd) {
    (void)fd;
    errno = ENOSYS;
    return -1;
}

int dup2(int fd, int target) {
    (void)fd;
    (void)target;
    errno = ENOSYS;
    return -1;
}

/* No hard links on lv2: link() publishes a copy instead, keeping link()'s "never replace an
 * existing file" guarantee. Callers publishing a temporary file unlink it afterwards. */
int link(const char* target, const char* linkpath) {
    const int in = open(target, O_RDONLY);
    if (in < 0)
        return -1;
    const int out = open(linkpath, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (out < 0) {
        close(in);
        return -1;
    }
    char buffer[4096];
    ssize_t n;
    while ((n = read(in, buffer, sizeof(buffer))) > 0) {
        if (write(out, buffer, (size_t)n) != n) {
            n = -1;
            break;
        }
    }
    close(in);
    if (close(out) != 0 || n < 0) {
        unlink(linkpath);
        return -1;
    }
    return 0;
}

/* lv2 refuses to rename over an existing file (CELL_EEXIST); POSIX replaces it.
 * Linked with -Wl,--wrap=rename. Not atomic: the target is gone briefly. */
int __real_rename(const char* from, const char* to);

int __wrap_rename(const char* from, const char* to) {
    if (__real_rename(from, to) == 0)
        return 0;
    if (access(to, F_OK) != 0 || unlink(to) != 0)
        return -1;
    return __real_rename(from, to);
}
