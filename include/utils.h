#ifndef UTILS_H
#define UTILS_H

#define CMDBUFLEN 100

#define sockaddr_addr(sockaddr) ((uint32_t)(((struct sockaddr_in *)addr)->sin_addr.s_addr))
#define sockaddr_port(sockaddr) ((uint16_t)(((struct sockaddr_in *)sockaddr)->sin_port))

#define print_debug(str, ...)                       \
    printf(str" - %s:%u\n", ##__VA_ARGS__, __FILE__, __LINE__);

#define print_err(str, ...)                     \
    fprintf(stderr, str, ##__VA_ARGS__);

int run_cmd(char *cmd, ...);
uint32_t sum_every_16bits(void *addr, int count);
uint16_t checksum(void *addr, int count, int start_sum);
int get_address(char *host, char *port, struct sockaddr *addr);
uint32_t parse_ipv4_string(char *addr);
uint32_t min(uint32_t x, uint32_t y);
void print_buffer_hexdump(void *buffer, int size);
void build_sockaddr_from_host_order(
    uint16_t srcport,          // 网络字节序端口
    uint32_t srcaddr,          // 网络字节序 IPv4 地址
    struct sockaddr *addr,     // 输出：sockaddr 指针
    socklen_t *addr_len              // 输出：地址结构长度指针
);
#endif
