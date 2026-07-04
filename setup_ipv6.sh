#!/bin/bash
#
# setup_ipv6.sh - Configure host environment for IPv6 testing with lvl-ip.
#
# Usage: sudo ./setup_ipv6.sh [setup|teardown]
#
# This script configures the TAP device (tap0) with an IPv6 address
# and enables IPv6 forwarding so that the lvl-ip userspace stack can
# communicate with the host over IPv6.
#
# Network topology:
#   Host (tap0: fd00::1/64) <--> lvl-ip (fd00::2/64, link-local auto)
#

set -eu

TAP_DEV="tap0"
IPV6_PREFIX="fd00::"
IPV6_HOST_ADDR="fd00::1/64"
IPV6_STACK_ADDR="fd00::2"

action="${1:-setup}"

function setup_ipv6 {
    echo "[setup] Enabling IPv6 forwarding..."
    sysctl -w net.ipv6.conf.all.forwarding=1

    echo "[setup] Enabling IPv6 on ${TAP_DEV}..."
    sysctl -w net.ipv6.conf.${TAP_DEV}.disable_ipv6=0

    echo "[setup] Adding IPv6 address ${IPV6_HOST_ADDR} to ${TAP_DEV}..."
    ip -6 addr add ${IPV6_HOST_ADDR} dev ${TAP_DEV} 2>/dev/null || true

    echo "[setup] Adding IPv6 route for stack address..."
    ip -6 route add ${IPV6_STACK_ADDR}/128 dev ${TAP_DEV} 2>/dev/null || true

    echo "[setup] Enabling IPv6 accept_ra on ${TAP_DEV}..."
    sysctl -w net.ipv6.conf.${TAP_DEV}.accept_ra=2

    echo "[setup] IPv6 environment ready."
    echo ""
    echo "Verify with:"
    echo "  ip -6 addr show dev ${TAP_DEV}"
    echo "  ip -6 route show dev ${TAP_DEV}"
    echo "  ping6 -c3 ${IPV6_STACK_ADDR}"
}

function teardown_ipv6 {
    echo "[teardown] Removing IPv6 routes..."
    ip -6 route del ${IPV6_STACK_ADDR}/128 dev ${TAP_DEV} 2>/dev/null || true

    echo "[teardown] Removing IPv6 address..."
    ip -6 addr del ${IPV6_HOST_ADDR} dev ${TAP_DEV} 2>/dev/null || true

    echo "[teardown] Disabling IPv6 forwarding..."
    sysctl -w net.ipv6.conf.all.forwarding=0

    echo "[teardown] IPv6 environment cleaned up."
}

case "$action" in
    setup)
        setup_ipv6
        ;;
    teardown)
        teardown_ipv6
        ;;
    *)
        echo "Usage: $0 [setup|teardown]"
        exit 1
        ;;
esac
