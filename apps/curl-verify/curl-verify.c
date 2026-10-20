/*
 * curl-verify: 拥塞控制测试专用收发客户端
 *
 * 下载模式: 响应体落盘 download.bin，Content-Length + SHA-256 校验
 *           （测 lvl-ip 接收路径：乱序 / dupACK / 接收窗口）
 * 上传模式: 发送指定文件，服务端回传 SHA-256 比对
 *           （测 lvl-ip 发送路径：重传 / RTO / cwnd）
 *
 * Usage:
 *   curl-verify HOST PORT [EXPECTED_SHA256]
 *   curl-verify -u HOST PORT FILE
 */

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>

#include "sha256.h"

#define MAX_HOSTNAME 50
#define RLEN 4096
#define HDRBUF_SIZE 5120
#define REPLY_SIZE 128

/* ═══ Static helpers (internal) ═══════════════════ */

static int get_address(char *host, char *port, struct sockaddr *addr)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *rp = NULL;
    int s = -1;

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

/* 循环 write，处理部分写
 * @return 成功 0，失败 -1 */
static int write_all(int sock, const uint8_t *data, int len)
{
    int off = 0;
    int n = 0;

    while (off < len) {
        n = write(sock, data + off, len - off);
        if (n <= 0) {
            perror("write error");
            return -1;
        }
        off += n;
    }

    return 0;
}

/* 定位 HTTP 响应头结束位置并解析 Content-Length
 * @return 头部总长度（含 \r\n\r\n）；头部未接收完整返回 -1 */
static int http_parse_header(const char *hdrbuf, long *content_length)
{
    const char *split = NULL;
    const char *p = NULL;

    split = strstr(hdrbuf, "\r\n\r\n");
    if (split == NULL) {
        return -1;
    }

    *content_length = -1;
    p = hdrbuf;
    while (p != NULL && p < split) {
        if (strncasecmp(p, "Content-Length:", 15) == 0) {
            *content_length = strtol(p + 15, NULL, 10);
        }
        p = strstr(p, "\r\n");
        if (p != NULL) {
            p += 2;
        }
    }

    return (int)(split - hdrbuf) + 4;
}

/* body 落盘并计入 sha256 与总字节数
 * @return 成功 0，失败 -1 */
static int write_body(FILE *fp, sha256_ctx *ctx, const uint8_t *data, int len, long *total)
{
    if (len <= 0) {
        return 0;
    }

    if (fwrite(data, 1, len, fp) != (size_t)len) {
        perror("file write error");
        return -1;
    }
    sha256_update(ctx, data, len);
    *total += len;

    return 0;
}

/* 传输完整性校验与结果输出
 * @return 校验通过 0，失败 1 */
static int verify_result(long total, long content_length, const char *hex, const char *expected)
{
    if (content_length >= 0 && total != content_length) {
        printf("Verify FAILED: received %ld bytes, Content-Length %ld\n", total, content_length);
        return 1;
    }

    if (expected[0] != '\0' && strcasecmp(hex, expected) != 0) {
        printf("Verify FAILED: sha256 mismatch\n  expected: %s\n  received: %s\n", expected, hex);
        return 1;
    }

    printf("Received %ld bytes, verify OK\n  sha256: %s\n", total, hex);

    return 0;
}

/* ═══ Global functions (public API) ═══════════════ */

/* 下载模式：GET / ，响应体落盘并校验 */
static int run_download(int argc, char **argv)
{
    struct sockaddr addr;
    sha256_ctx ctx = {0};
    uint8_t hash[SHA256_HASH_LEN] = {0};
    char hex[SHA256_HEX_LEN] = {0};
    char expected[SHA256_HEX_LEN] = {0};
    char str[512] = {0};
    char buf[RLEN] = {0};
    char hdrbuf[HDRBUF_SIZE] = {0};
    int sock = -1;
    int len = 0;
    int rlen = 0;
    int hlen = 0;
    int header_done = 0;
    int body_off = 0;
    long total = 0;
    long content_length = -1;
    FILE *fp = NULL;

    if (strnlen(argv[1], MAX_HOSTNAME) == MAX_HOSTNAME || strnlen(argv[2], 6) == 6) {
        printf("HOST or PORT invalid\n");
        return 1;
    }

    if (get_address(argv[1], argv[2], &addr) != 0) {
        printf("could not resolve hostname\n");
        return 1;
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);

    if (connect(sock, &addr, sizeof(addr)) == -1) {
        perror("could not establish connection");
        return 1;
    }

    snprintf(str, 512, "GET / HTTP/1.1\r\nHost: %s:%s\r\nConnection: close\r\n\r\n", argv[1], argv[2]);
    len = strlen(str);

    if (write_all(sock, (uint8_t *)str, len) != 0) {
        close(sock);
        return 1;
    }

    fp = fopen("download.bin", "wb");
    if (fp == NULL) {
        perror("could not open download.bin");
        close(sock);
        return 1;
    }
    sha256_init(&ctx);

    while ((rlen = read(sock, buf, RLEN)) > 0) {
        if (!header_done) {
            if (hlen + rlen >= HDRBUF_SIZE) {
                printf("HTTP header too large\n");
                fclose(fp);
                close(sock);
                return 1;
            }
            memcpy(hdrbuf + hlen, buf, rlen);
            hlen += rlen;
            hdrbuf[hlen] = '\0';

            body_off = http_parse_header(hdrbuf, &content_length);
            if (body_off < 0) {
                continue;
            }
            header_done = 1;
            if (write_body(fp, &ctx, (uint8_t *)hdrbuf + body_off, hlen - body_off, &total) != 0) {
                fclose(fp);
                close(sock);
                return 1;
            }
            continue;
        }

        if (write_body(fp, &ctx, (uint8_t *)buf, rlen, &total) != 0) {
            fclose(fp);
            close(sock);
            return 1;
        }
    }

    if (rlen == -1) {
        perror("read error");
        fclose(fp);
        close(sock);
        return 1;
    }

    fclose(fp);
    sha256_final(&ctx, hash);
    sha256_hex(hash, hex);
    if (argc == 4) {
        snprintf(expected, SHA256_HEX_LEN, "%s", argv[3]);
    }

    len = verify_result(total, content_length, hex, expected);
    close(sock);

    return len;
}

/* 上传模式：发送 PUT <size>\n + 文件原始字节，比对服务端回传的 sha256 */
static int run_upload(char *host, char *port, char *file)
{
    struct sockaddr addr;
    sha256_ctx ctx = {0};
    uint8_t hash[SHA256_HASH_LEN] = {0};
    uint8_t buf[RLEN] = {0};
    char hex[SHA256_HEX_LEN] = {0};
    char hdr[64] = {0};
    char reply[REPLY_SIZE] = {0};
    int sock = -1;
    int rlen = 0;
    long total = 0;
    long size = 0;
    FILE *fp = NULL;

    if (strnlen(host, MAX_HOSTNAME) == MAX_HOSTNAME || strnlen(port, 6) == 6) {
        printf("HOST or PORT invalid\n");
        return 1;
    }

    fp = fopen(file, "rb");
    if (fp == NULL) {
        perror("could not open upload file");
        return 1;
    }

    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (get_address(host, port, &addr) != 0) {
        printf("could not resolve hostname\n");
        fclose(fp);
        return 1;
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);

    if (connect(sock, &addr, sizeof(addr)) == -1) {
        perror("could not establish connection");
        fclose(fp);
        return 1;
    }

    snprintf(hdr, sizeof(hdr), "PUT %ld\n", size);
    if (write_all(sock, (uint8_t *)hdr, strlen(hdr)) != 0) {
        fclose(fp);
        close(sock);
        return 1;
    }

    sha256_init(&ctx);
    while ((rlen = fread(buf, 1, RLEN, fp)) > 0) {
        if (write_all(sock, buf, rlen) != 0) {
            fclose(fp);
            close(sock);
            return 1;
        }
        sha256_update(&ctx, buf, rlen);
        total += rlen;
    }
    fclose(fp);

    if (total != size) {
        printf("Verify FAILED: read %ld bytes, expect %ld\n", total, size);
        close(sock);
        return 1;
    }

    sha256_final(&ctx, hash);
    sha256_hex(hash, hex);

    rlen = read(sock, reply, sizeof(reply) - 1);
    if (rlen <= 0) {
        perror("read upload reply");
        close(sock);
        return 1;
    }
    reply[rlen] = '\0';

    /* 期望回复: "OK <64位小写hex>\n" */
    if (strncmp(reply, "OK ", 3) != 0 ||
        strncasecmp(reply + 3, hex, SHA256_HASH_LEN * 2) != 0) {
        printf("Verify FAILED: server reply mismatch\n  local  : %s\n  server : %s\n", hex, reply);
        close(sock);
        return 1;
    }

    printf("Uploaded %ld bytes, verify OK\n  sha256: %s\n", total, hex);
    close(sock);

    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 5 && strcmp(argv[1], "-u") == 0) {
        return run_upload(argv[2], argv[3], argv[4]);
    }

    if (argc == 3 || argc == 4) {
        return run_download(argc, argv);
    }

    printf("Usage:\n");
    printf("  curl-verify HOST PORT [EXPECTED_SHA256]    download (GET /)\n");
    printf("  curl-verify -u HOST PORT FILE              upload FILE\n");

    return 1;
}
