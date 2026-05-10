#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
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

static int dump_recv_data(int dump_fd, const char *buf, ssize_t len)
{
    ssize_t dumped = 0;
    ssize_t n = 0;

    if (dump_fd < 0) {
        return 0;
    }

    while (dumped < len) {
        n = write(dump_fd, buf + dumped, len - dumped);
        if (n < 0) {
            perror("dump write");
            return -1;
        }
        dumped += n;
    }

    return 0;
}

int echo_task(int fd)
{
    char buf[4096]; // 增大一点缓冲区，减少系统调用次数
    ssize_t readSize; // 注意用 ssize_t
    ssize_t writeSize;
    int dump_fd = -1;
    size_t dump_off = 0;

    // 建议：关闭 stdout 缓冲，防止日志堵塞
    setvbuf(stdout, NULL, _IONBF, 0);

    /* 每次连接覆盖写入 test.txt，便于和源文件 diff 定位错位 */
    dump_fd = open("test.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dump_fd < 0) {
        perror("open test.txt");
    }

    printf("start echo task fd: %d\n", fd);

    while((readSize = read(fd, buf, sizeof(buf))) > 0) {
        // 不要打印 buf 的内容，因为 urandom 是乱码，打印出来会刷屏且极慢
        printf("recv size: %ld off: %zu\n", readSize, dump_off);

        dump_recv_data(dump_fd, buf, readSize);
        dump_off += readSize;

        // 必须确保 write 写入了所有读取到的字节
        size_t bytes_to_write = readSize;
        size_t bytes_written = 0;

        while (bytes_written < bytes_to_write) {
            writeSize = write(fd, buf + bytes_written, bytes_to_write - bytes_written);
            if (writeSize < 0) {
                perror("write error");
                if (dump_fd >= 0) {
                    close(dump_fd);
                }
                close(fd);
                return -1;
            }
            bytes_written += writeSize;
        }
    }

    if (readSize < 0) {
        perror("read error");
    } else {
        printf("client disconnected, echo finished. total dumped: %zu\n", dump_off);
    }

    if (dump_fd >= 0) {
        close(dump_fd);
    }
    close(fd);
    exit(EXIT_SUCCESS);
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

        if ((pid = fork()) > 0) {
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
