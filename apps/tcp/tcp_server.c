#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>

#define MAX_HOSTNAME 50
#define RLEN 4096

int get_address(char *host, char *port, struct sockaddr *addr)
{
    struct addrinfo hints;
    struct addrinfo *result, *rp;
    int s;

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    s = getaddrinfo(host, port, &hints, &result);

    if (s != 0) {
        printf("getaddrinfo: %s\n", gai_strerror(s));
        exit(EXIT_FAILURE);
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        *addr = *rp->ai_addr;
        freeaddrinfo(result);
        return 0;
    }
    
    return 1;
}

void print_sockaddr(const struct sockaddr *addr, socklen_t addrlen) 
{
    char ip_str[INET6_ADDRSTRLEN]; // 足够容纳 IPv4 和 IPv6 的字符串
    in_port_t port;

    if (addr == NULL) {
        printf("NULL address\n");
        return;
    }

    switch (addr->sa_family) {
        case AF_INET: {
            const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
            inet_ntop(AF_INET, &sin->sin_addr, ip_str, sizeof(ip_str));
            port = ntohs(sin->sin_port);
            printf("IPv4: %s:%d\n", ip_str, port);
            break;
        }
        case AF_INET6: {
            const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)addr;
            inet_ntop(AF_INET6, &sin6->sin6_addr, ip_str, sizeof(ip_str));
            port = ntohs(sin6->sin6_port);
            printf("IPv6: [%s]:%d\n", ip_str, port);
            break;
        }
        default:
            printf("Unknown address family: %d\n", addr->sa_family);
            break;
    }
}

int echo_task(int fd)
{
    char buf[1024] = {};
    size_t readSize = 0;

    printf("start echo task fd: %d\n", fd);
    while((readSize = read(fd, buf, sizeof(buf))) > 0) {
        printf("recv: %s, size: %ld\n", buf, readSize);
        if (write(fd, buf, readSize) < 0) {
            printf("write error: %s\n", strerror(errno));
            close(fd);
            return -1;
        }
    }

    while (readSize < 0) {
        printf("read error %s\n", strerror(errno));
        return -1;
    }

    close(fd);
    exit(EXIT_SUCCESS);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3 || strnlen(argv[1], MAX_HOSTNAME) == MAX_HOSTNAME) {
        printf("called but HOST or PORT not given or invalid\n");
        return 1;
    }

    struct sockaddr srcaddr, bindaddr, acceptaddr;
    socklen_t addrlen = sizeof(acceptaddr);
    int sock;
    pid_t pid = 0;

    if (strnlen(argv[2], 6) == 6) {
        printf("Curl called but PORT malformed\n");
        return 1;
    }

    if (get_address(argv[1], argv[2], &bindaddr) != 0) {
        printf("Curl could not resolve hostname\n");
        return 1;
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);

    char buf[RLEN] = { 0 };
    int rlen = 0;
    socklen_t srcaddrLen = sizeof(struct sockaddr);

    if (bind(sock, &bindaddr, sizeof(bindaddr)) < 0) {
        perror("error bind addr");
        close(sock);
        return 1;
    }

    if (listen(sock, 10) < 0) {
        printf("listen error %s\n", strerror(errno));
        close(sock);
        return 1;
    }

    for (;;) {
        int acceptFd = accept(sock, &acceptaddr, &addrlen);
        if (acceptFd < 0) {
            printf("acccept error: %s\n", strerror(errno));
            break;
        }

        if (pid = fork() > 0) {
            print_sockaddr(&acceptaddr, addrlen);
        } else if (pid == 0) {
            echo_task(acceptFd);
        } else {
            printf("fork error: %s\n", strerror(errno));
        }
    }

    close(sock);

    return 0;
}
