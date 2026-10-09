/* PS3 wrapper: lv2 has no POSIX signal masks, so the sig* jump pair is plain
 * setjmp/longjmp. */
#ifndef MKW_PS3_SETJMP_H
#define MKW_PS3_SETJMP_H
#include_next <setjmp.h>
#ifndef sigsetjmp
typedef jmp_buf sigjmp_buf;
#define sigsetjmp(env, savemask) setjmp(env)
#define siglongjmp(env, val) longjmp(env, val)
#endif
#endif
