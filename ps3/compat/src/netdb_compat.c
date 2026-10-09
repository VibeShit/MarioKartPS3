/* IPv4-only getaddrinfo/freeaddrinfo for lv2 (see compat/include/netdb.h). */
#include <netdb.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static struct addrinfo* make_entry(struct in_addr address, unsigned short port, const struct addrinfo* hints) {
    struct addrinfo* info = (struct addrinfo*)calloc(1, sizeof(struct addrinfo) + sizeof(struct sockaddr_in));
    if (info == NULL)
        return NULL;
    struct sockaddr_in* sin = (struct sockaddr_in*)(info + 1);
    sin->sin_family = AF_INET;
    sin->sin_port = htons(port);
    sin->sin_addr = address;
    info->ai_family = AF_INET;
    info->ai_socktype = hints != NULL ? hints->ai_socktype : 0;
    info->ai_protocol = hints != NULL ? hints->ai_protocol : 0;
    info->ai_addrlen = sizeof(struct sockaddr_in);
    info->ai_addr = (struct sockaddr*)sin;
    return info;
}

int getaddrinfo(const char* node, const char* service, const struct addrinfo* hints, struct addrinfo** res) {
    if (res == NULL)
        return EAI_FAIL;
    *res = NULL;
    if (hints != NULL && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET)
        return EAI_FAMILY;

    unsigned short port = 0;
    if (service != NULL && *service != '\0') {
        char* end = NULL;
        const long value = strtol(service, &end, 10);
        if (end == service || *end != '\0' || value < 0 || value > 65535)
            return EAI_SERVICE;
        port = (unsigned short)value;
    }

    struct in_addr address;
    if (node == NULL) {
        address.s_addr = htonl((hints != NULL && (hints->ai_flags & AI_PASSIVE)) ? INADDR_ANY : INADDR_LOOPBACK);
        *res = make_entry(address, port, hints);
        return *res != NULL ? 0 : EAI_MEMORY;
    }
    if (inet_aton(node, &address)) {
        *res = make_entry(address, port, hints);
        return *res != NULL ? 0 : EAI_MEMORY;
    }
    if (hints != NULL && (hints->ai_flags & AI_NUMERICHOST))
        return EAI_NONAME;

    struct hostent* host = gethostbyname(node);
    if (host == NULL || host->h_addrtype != AF_INET || host->h_addr_list == NULL)
        return EAI_NONAME;
    struct addrinfo* head = NULL;
    struct addrinfo** tail = &head;
    for (char** entry = host->h_addr_list; *entry != NULL; ++entry) {
        memcpy(&address, *entry, sizeof(address));
        struct addrinfo* info = make_entry(address, port, hints);
        if (info == NULL) {
            freeaddrinfo(head);
            return EAI_MEMORY;
        }
        *tail = info;
        tail = &info->ai_next;
    }
    *res = head;
    return head != NULL ? 0 : EAI_NONAME;
}

void freeaddrinfo(struct addrinfo* res) {
    while (res != NULL) {
        struct addrinfo* next = res->ai_next;
        free(res);
        res = next;
    }
}

const char* gai_strerror(int errcode) {
    switch (errcode) {
    case EAI_NONAME:
        return "name does not resolve";
    case EAI_SERVICE:
        return "unsupported service";
    case EAI_FAMILY:
        return "unsupported address family";
    case EAI_MEMORY:
        return "out of memory";
    default:
        return "name resolution failed";
    }
}
