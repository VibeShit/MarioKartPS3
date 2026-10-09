/* PS3 wrapper: lv2 networking has gethostbyname but no getaddrinfo, which the
 * runtime's IOS socket HLE uses. compat/src/netdb_compat.c provides an
 * IPv4-only getaddrinfo on top of inet_aton/gethostbyname. */
#ifndef MKW_PS3_NETDB_H
#define MKW_PS3_NETDB_H
#include_next <netdb.h>
#ifdef __cplusplus
extern "C" {
#endif
#ifndef EAI_NONAME
#define EAI_BADFLAGS -1
#define EAI_NONAME -2
#define EAI_AGAIN -3
#define EAI_FAIL -4
#define EAI_FAMILY -6
#define EAI_SOCKTYPE -7
#define EAI_SERVICE -8
#define EAI_MEMORY -10
#endif
#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif
int getaddrinfo(const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** res);
void freeaddrinfo(struct addrinfo* res);
const char* gai_strerror(int errcode);
#ifdef __cplusplus
}
#endif
#endif
