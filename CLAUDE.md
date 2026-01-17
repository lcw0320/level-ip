# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Level-IP is a **Linux userspace TCP/IP stack** implementation using TUN/TAP devices. This is an educational project designed to understand TCP/IP networking, Linux systems programming, and the Socket API by implementing the protocols from scratch.

**Important**: This is NOT production-ready code. It's a learning project with hardcoded values and simplified implementations.

## Build and Development Commands

### Building

```bash
# Build the main lvl-ip daemon
make

# Build with debug symbols and sanitizers (recommended for development)
make debug

# Build everything (daemon + example apps)
make all

# Clean build artifacts
make clean
```

### Testing

```bash
# Run all test suites (requires root privileges)
make test

# Run specific test suite
cd tests
./suites/arp/suite-arp
```

### Running

```bash
# Start the userspace TCP/IP stack daemon
./lvl-ip

# Use with applications via the wrapper library
cd tools
./level-ip curl google.com 80
./level-ip firefox google.com
```

## Architecture Overview

### Multi-threaded Design

Level-IP runs as a daemon with 4 main threads (see [main.c:23-29](src/main.c#L23-L29)):

1. **THREAD_CORE**: Receives packets from TAP device (`netdev_rx_loop`)
2. **THREAD_TIMERS**: Handles TCP timers (retransmission, timeouts)
3. **THREAD_IPC**: Listens for socket API calls from applications via Unix sockets
4. **THREAD_SIGNAL**: Handles termination signals (SIGINT, SIGQUIT)

### Protocol Stack Layers

The stack is organized in traditional OSI layers from bottom to top:

```
Application (via liblevelip.so wrapper)
    ↕ (IPC via Unix sockets)
Socket API (socket.c, sock.c)
    ↕
TCP Layer (tcp.c, tcp_input.c, tcp_output.c, tcp_data.c)
    ↕
IP Layer (ip_input.c, ip_output.c, route.c, dst.c)
    ↕
Link Layer (arp.c, ethernet.h, netdev.c)
    ↕
TUN/TAP Device (tuntap_if.c)
```

### Key Components

- **IPC Communication** ([ipc.c](src/ipc.c)): Applications communicate with lvl-ip daemon through Unix domain sockets. The wrapper library `liblevelip.so` intercepts standard socket API calls and forwards them via IPC.

- **Socket Management** ([socket.c](src/socket.c), [sock.c](src/sock.c)): Implements socket abstraction with operations (connect, read, write, close). Each socket has associated protocol-specific data (e.g., `tcp_sock`).

- **TCP State Machine** ([tcp.c](src/tcp.c)): Implements RFC 793 TCP states (LISTEN, SYN_SENT, ESTABLISHED, etc.) with proper transitions.

- **TCP Data Handling** ([tcp_data.c](src/tcp_data.c)): Manages out-of-order segments, receive/send queues using linked lists ordered by sequence numbers.

- **Packet Buffers** ([skbuff.c](src/skbuff.c)): Socket buffers (skb) structure similar to Linux kernel's sk_buff for packet handling.

- **ARP Cache** ([arp.c](src/arp.c)): Simple ARP request/reply handling with caching for Ethernet address resolution.

- **Routing** ([route.c](src/route.c)): Currently hardcoded single route table with default device (10.0.0.4/24).

### Data Structures

- **Linked Lists** ([list.h](include/list.h)): Linux kernel-style intrusive doubly linked lists used throughout (socket lists, packet queues, etc.)
- **Wait Queues** ([wait.h](include/wait.h)): Synchronization mechanism for blocking socket operations
- **Timers** ([timer.c](src/timer.c)): Timer wheel implementation for TCP retransmission and timeouts

## Development Guidelines

### Debug Builds

Always use `make debug` during development. It enables:
- Debug symbols for GDB
- Thread Sanitizer (`-fsanitize=thread`) to catch race conditions
- Debug macros (`-DDEBUG_TCP`, `-DDEBUG_SOCKET`) for verbose logging

### Enabling Specific Debug Output

Debug output is controlled by compile-time macros:

```bash
# Enable socket debugging
make clean
CFLAGS+=-DDEBUG_SOCKET make debug

# Enable TCP debugging
CFLAGS+=-DDEBUG_TCP make debug
```

### Debugging with tcpdump

Monitor traffic on the TAP interface:

```bash
tcpdump -i any host 10.0.0.4 -n
```

### Required Capabilities

The lvl-ip binary needs `CAP_NET_ADMIN` capability (automatically set by Makefile). The daemon drops this capability after initialization for security ([main.c:110-121](src/main.c#L110-L121)).

The `ip` tool also needs capabilities:
```bash
sudo setcap cap_net_admin=ep /usr/bin/ip
```

## Code Patterns to Follow

### Variable Declarations

All variable declarations must be at the beginning of functions (C89 style), NOT within if/for blocks. Exception: loop counters in for/while loops.

### Control Flow

**Mandatory**: All `if` and loop statements MUST use braces `{}`, even for single statements.

```c
// Correct
if (condition) {
    do_something();
}

// Wrong
if (condition)
    do_something();
```

### Function Size

Keep functions under 100 lines. If a function grows beyond this, extract logical blocks into separate functions.

### Minimal Changes

When fixing bugs or adding features, minimize changes to existing code. If adding substantial logic to a function, extract it into a new helper function rather than modifying the original.

### Avoid Code Duplication

If you see repeated code patterns, refactor into a common function. This prevents maintenance issues.

## Common Hardcoded Values

Be aware of these limitations in the current implementation:
- IP address: 10.0.0.4/24 (hardcoded)
- Single TAP interface (tap0)
- Single route table entry
- TCP window size: 512 bytes
- Maximum Segment Size (MSS): 1460 bytes

## Testing Requirements

Tests are end-to-end tests that use the host Linux applications. To run tests:

1. Ensure `arping` and `tc` have network capabilities
2. Run `make test` from project root
3. Individual test suites are in `tests/suites/`

## Implementation Status

**Implemented**:
- Ethernet II frame handling
- ARP request/reply with caching
- IPv4 packet handling with checksums
- ICMP echo request/reply
- TCP three-way handshake (SYN, SYN-ACK, ACK)
- TCP data transmission and reception
- TCP retransmission (RFC 6298)
- TCP user timeout (RFC 793)
- Socket API: socket(), connect(), read(), write(), close(), poll()

**Not Yet Implemented**:
- Server socket operations (bind, listen, accept)
- IP fragmentation
- TCP window management improvements
- TCP congestion control
- Selective Acknowledgments (SACK) - partial implementation exists
- Raw sockets

## Related Blog Posts

The development is documented in a series of blog posts:
- Part 1: Ethernet & ARP
- Part 2: IPv4 & ICMPv4
- Part 3: TCP Basics & Handshake
- Part 4: TCP Data Flow & Socket API
- Part 5: TCP Retransmission

See README.md for links.
