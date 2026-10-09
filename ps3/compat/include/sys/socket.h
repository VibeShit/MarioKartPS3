/* PS3 wrapper: adds the bits of the POSIX socket header lv2 omits. */
#ifndef MKW_PS3_SYS_SOCKET_H
#define MKW_PS3_SYS_SOCKET_H
#include_next <sys/socket.h>
#include <sys/time.h>
#ifndef MSG_NOSIGNAL
/* lv2 never raises SIGPIPE, so there is nothing to suppress. */
#define MSG_NOSIGNAL 0
#endif
#endif
