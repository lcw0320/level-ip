/*
 * curl-v6.c — IPv6 TCP client test app for level-ip TDD.
 *
 * Connects to an IPv6 server, calls getpeername(), sends an HTTP GET,
 * prints the response.  Designed to expose TCP/IPv6 client-path bugs.
 *
 * Usage: curl-v6 <ipv6-address> <port>
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define MAX_HOSTNAME 50
#define RLEN 4096

static void print_peer_info(int sock)
{
    struct sockaddr_storage peer;
    socklen_t peerlen = sizeof(peer);
    char addr_str[INET6_ADDRSTRLEN];

    if (getpeername(sock, (struct sockaddr *)&peer, &peerlen) != 0) {
        printf("getpeername: failed\n");
        return;
    }

    printf("getpeername: family=%d", peer.ss_family);

    if (peer.ss_family == AF_INET6) {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&peer;
        inet_ntop(AF_INET6, &sin6->sin6_addr, addr_str, sizeof(addr_str));
        printf(" addr=[%s]:%d (AF_INET6)", addr_str, ntohs(sin6->sin6_port));
    } else if (peer.ss_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&peer;
        inet_ntop(AF_INET, &sin->sin_addr, addr_str, sizeof(addr_str));
        printf(" addr=%s:%d (AF_INET)", addr_str, ntohs(sin->sin_port));
    } else {
        printf(" addr=unknown-family");
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *rp = NULL;
    int sock = -1;
    int s = 0;
    char str[512];
    char buf[RLEN];
    int len = 0;
    int rlen = 0;

    if (argc != 3 || strnlen(argv[1], MAX_HOSTNAME) == MAX_HOSTNAME) {
        printf("Usage: curl-v6 <ipv6-address> <port>\n");
        return 1;
    }

    if (strnlen(argv[2], 6) == 6) {
        printf("curl-v6: PORT malformed\n");
        return 1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET6;
    hints.ai_socktype = SOCK_STREAM;

    s = getaddrinfo(argv[1], argv[2], &hints, &result);
    if (s != 0) {
        printf("getaddrinfo: %s\n", gai_strerror(s));
        return 1;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock == -1) {
            continue;
        }

        if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }

        close(sock);
        sock = -1;
    }

    freeaddrinfo(result);

    if (sock < 0) {
        printf("curl-v6: could not connect to [%s]:%s\n", argv[1], argv[2]);
        return 1;
    }

    /* Print getpeername result — this is the key assertion point */
    print_peer_info(sock);

    snprintf(str, sizeof(str),
             "GET / HTTP/1.1\r\nHost: [%s]:%s\r\nConnection: close\r\n\r\n",
             argv[1], argv[2]);
    len = strlen(str);

    if (write(sock, str, len) != len) {
        printf("Write error\n");
        close(sock);
        return 1;
    }

    memset(buf, 0, sizeof(buf));
    while ((rlen = read(sock, buf, RLEN)) > 0) {
        printf("%s", buf);
        memset(buf, 0, sizeof(buf));
    }

    if (rlen == -1) {
        perror("curl-v6: read error");
    }

    close(sock);
    return 0;
}
