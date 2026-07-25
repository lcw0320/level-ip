# Tests

End-to-end test suite for level-ip. Host Linux applications exercise traffic
flow through the userspace TCP/IP stack running on a TAP device.

## Usage

```bash
# Full test (all groups)
make test

# Or directly
sudo bash tests/test-run.sh

# Skip netem tests (faster)
sudo bash tests/test-run.sh --quick
```

Root privileges are required.

## Test Groups

| Group | Description |
|-------|-------------|
| G1 | IPv4 regression (ping, ARP) |
| G2 | TCP/IPv4 (curl sync/poll, connection refused, server echo) |
| G3 | TCP netem (delay, loss, duplication) — skipped with `--quick` |
| G4 | IPv6 network layer (ping6, NDP, DAD, default route) |
| G5 | TCP/IPv6 TDD red-light (expected failures, runs last) |

## Directory Structure

```
tests/
├── test-run.sh          # Main orchestrator
├── lib/                 # Test group modules
│   ├── common.sh        # Shared helpers (pass/fail, cleanup, start_stack)
│   ├── g1-ipv4.sh
│   ├── g2-tcp.sh
│   ├── g3-netem.sh
│   ├── g4-ipv6.sh
│   └── g5-tcp6.sh
├── fixtures/            # Golden test data
│   └── curl-fixture.txt
├── send_ra.py           # IPv6 Router Advertisement helper
└── README.md
```
