/*
 * server.c — IPv4 TCP echo server for level-ip testing.
 *
 * Binds to the given host:port, accepts ONE connection,
 * echoes all data back to the client, then exits.
 *
 * No fork() — avoids PID mismatch with liblevelip.so IPC.
 *
 * Usage: server <host> <port>
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>

#define BUF_SIZE 4096

static int resolve_address(const char *host, const char *port, struct sockaddr *addr)
{
    struct addrinfo hints;
    struct addrinfo *result;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &result) != 0) {
        return -1;
    }

    memcpy(addr, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);
    return 0;
}

int main(int argc, char **argv)
{
    int listen_fd = -1;
    int accept_fd = -1;
    struct sockaddr bind_addr;
    struct sockaddr_storage peer_addr;
    socklen_t peer_addrlen = sizeof(peer_addr);
    char buf[BUF_SIZE];
    char peer_str[INET_ADDRSTRLEN];
    ssize_t nread = 0;
    ssize_t nwritten = 0;
    ssize_t total = 0;
    int one = 1;

    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc != 3) {
        printf("Usage: %s <host> <port>\n", argv[0]);
        return 1;
    }

    if (resolve_address(argv[1], argv[2], &bind_addr) != 0) {
        printf("Could not resolve %s:%s\n", argv[1], argv[2]);
        return 1;
    }

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    if (bind(listen_fd, &bind_addr, sizeof(struct sockaddr_in)) < 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, 5) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    printf("LISTENING: %s port %s\n", argv[1], argv[2]);

    memset(&peer_addr, 0, sizeof(peer_addr));
    accept_fd = accept(listen_fd, (struct sockaddr *)&peer_addr, &peer_addrlen);
    if (accept_fd < 0) {
        perror("accept");
        close(listen_fd);
        return 1;
    }

    if (peer_addr.ss_family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&peer_addr;
        inet_ntop(AF_INET, &sin->sin_addr, peer_str, sizeof(peer_str));
        printf("ACCEPT: %s:%d\n", peer_str, ntohs(sin->sin_port));
    }

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
