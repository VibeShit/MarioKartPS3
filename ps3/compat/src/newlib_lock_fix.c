/* PSL1GHT fills newlib's syscall table and creates the meta lock that guards lazy
 * lock creation in priority 103-105 constructors, but GCC 13's libstdc++ sets up
 * the iostreams in a priority-90 constructor that already takes newlib locks
 * (__cxa_atexit), so the first lock allocation abort()s before main().
 *
 * Linked with -Wl,--wrap=__libc_auto_lock_allocate, this version skips allocation
 * until the syscall table exists (only one thread runs that early; newlib ignores
 * the failed lock/unlock) and otherwise creates its meta lock on first use. */
#include <stdint.h>
#include <stdlib.h>
#include <sys/lock.h>
#include <sys/syscalls.h>

typedef struct {
    uint32_t protocol;
    uint32_t recursive;
    char name[8];
} mkw_lwmutex_attr;

/* Same attributes as newlib's __libc_lock_attributes (priority protocol, recursive). */
static const mkw_lwmutex_attr kLockAttributes = {0x0002, 0x0010, ""};

extern int32_t sys_lwmutex_create(sys_lwmutex_t* lwmutex, const void* attr);
extern int32_t sys_lwmutex_lock(sys_lwmutex_t* lwmutex, uint64_t timeout);
extern int32_t sys_lwmutex_unlock(sys_lwmutex_t* lwmutex);

static sys_lwmutex_t g_metaLock;

void __wrap___libc_auto_lock_allocate(sys_lwmutex_t* lwmutex) {
    if (!__syscalls.sys_lwmutex_create_r)
        return;
    if (!g_metaLock.attribute && sys_lwmutex_create(&g_metaLock, &kLockAttributes))
        abort();
    if (sys_lwmutex_lock(&g_metaLock, 0))
        abort();
    if (!lwmutex->attribute && sys_lwmutex_create(lwmutex, &kLockAttributes))
        abort();
    if (sys_lwmutex_unlock(&g_metaLock))
        abort();
}
