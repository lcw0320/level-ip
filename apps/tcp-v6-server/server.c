/*
 * server.c — IPv6 TCP echo server test app for level-ip TDD.
 *
 * Binds to [::] on the given port, accepts ONE connection,
 * prints getpeername() of the accepted socket, echoes all data
 * back to the client, then exits.
 *
 * No fork() — avoids PID mismatch with liblevelip.so IPC.
 *
 * Usage: server <port>
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#define BUF_SIZE 4096

static void print_peer_info(int fd)
{
    struct sockaddr_storage peer;
    socklen_t peerlen = sizeof(peer);
    char addr_str[INET6_ADDRSTRLEN];

    memset(&peer, 0, sizeof(peer));

    if (getpeername(fd, (struct sockaddr *)&peer, &peerlen) != 0) {
        printf("ACCEPT_PEER: getpeername failed: %s\n", strerror(errno));
        return;
    }

    printf("ACCEPT_PEER: family=%d", peer.ss_family);

    if (peer.ss_family == AF_INET6) {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&peer;
        inet_ntop(AF_INET6, &sin6->sin6_addr, addr_str, sizeof(addr_str));
        printf(" addr=[%s]:%d (AF_INET6)\n", addr_str, ntohs(sin6->sin6_port));
    } else if (peer.ss_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&peer;
        inet_ntop(AF_INET, &sin->sin_addr, addr_str, sizeof(addr_str));
        printf(" addr=%s:%d (AF_INET)\n", addr_str, ntohs(sin->sin_port));
    } else {
        printf(" addr=unknown-family addrlen=%d\n", peerlen);
    }
}

int main(int argc, char **argv)
{
    int listen_fd = -1;
    int accept_fd = -1;
    int port = 0;
    struct sockaddr_in6 bind_addr;
    struct sockaddr_storage peer_addr;
    socklen_t peer_addrlen = sizeof(peer_addr);
    char buf[BUF_SIZE];
    ssize_t nread = 0;
    ssize_t nwritten = 0;
    ssize_t total = 0;
    int one = 1;

    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc != 2) {
        printf("Usage: %s <port>\n", argv[0]);
        return 1;
    }

    port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        printf("Invalid port: %s\n", argv[1]);
        return 1;
    }

    listen_fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin6_family = AF_INET6;
    bind_addr.sin6_port = htons((uint16_t)port);
    bind_addr.sin6_addr = in6addr_any;

    if (bind(listen_fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, 5) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    printf("LISTENING: [::] port %d\n", port);

    memset(&peer_addr, 0, sizeof(peer_addr));
    accept_fd = accept(listen_fd, (struct sockaddr *)&peer_addr, &peer_addrlen);
    if (accept_fd < 0) {
        perror("accept");
        close(listen_fd);
        return 1;
    }

    printf("ACCEPT: connection received\n");

    /* Print what accept() returned as peer address */
    print_peer_info(accept_fd);

    /* Echo loop */
    while ((nread = read(accept_fd, buf, BUF_SIZE)) > 0) {
        printf("ECHO: received %zd bytes\n", nread);
        total += nread;

        nwritten = 0;
        while (nwritten < nread) {
            ssize_t w = write(accept_fd, buf + nwritten, nread - nwritten);
            if (w < 0) {
                perror("write");
                close(accept_fd);
                close(listen_fd);
                return 1;
            }
            nwritten += w;
        }
    }

    printf("ECHO_DONE: total %zd bytes echoed\n", total);

    close(accept_fd);
    close(listen_fd);
    return 0;
}
