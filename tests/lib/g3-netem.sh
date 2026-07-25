#!/bin/bash
# g3-netem.sh — G3: TCP netem 测试 (delay, loss, duplication)

run_g3_netem() {
    if [ "$QUICK" = "1" ]; then
        group "G3: TCP netem (跳过)"
        skip "curl sync (delay)"
        skip "curl sync (loss)"
        skip "curl sync (duplication)"
        return
    fi

    group "G3: TCP netem"

    _netem_delay_loss
    _netem_duplication
}

_netem_delay_loss() {
    local cond name classid classnum netem port resp

    for cond in "delay:1:1:delay 2000ms:8002" "loss:1:2:loss 25%:8004"; do
        IFS=: read -r name classid classnum netem port <<< "$cond"

        python3 -m http.server "$port" --directory "$SCRIPT_DIR" >/dev/null 2>&1 &
        HTTP_PIDS="$HTTP_PIDS $!"

        tc class add dev "$TAP_DEV" parent 1: classid "1:$classid" htb rate 100mbit 2>/dev/null || true
        tc filter add dev "$TAP_DEV" parent 1: protocol ip prio 1 u32 \
            flowid "1:$classid" match ip sport "$port" 0xffff 2>/dev/null || true
        tc filter add dev "$TAP_DEV" parent 1: protocol ip prio 1 u32 \
            flowid "1:$classid" match ip dport "$port" 0xffff 2>/dev/null || true
        tc qdisc add dev "$TAP_DEV" parent "1:$classid" netem $netem 2>/dev/null || true
        sleep 2

        resp=$(timeout 30 "$REPO_DIR/tools/level-ip" "$REPO_DIR/apps/curl/curl" \
            10.0.0.5 "$port" 2>/dev/null \
            | sed 's/^Date:.*/Date:/' | sed 's/^Server:.*/Server:/') || true
        if diff -q "$FIXTURE" <(echo "$resp") >/dev/null 2>&1; then
            pass "curl sync ($name)"
        else
            fail "curl sync ($name)"
        fi

        # 清理该条件的 netem
        tc qdisc del dev "$TAP_DEV" parent "1:$classid" 2>/dev/null || true
        tc filter del dev "$TAP_DEV" parent 1: prio 1 2>/dev/null || true
        tc class del dev "$TAP_DEV" parent 1: classid "1:$classid" 2>/dev/null || true
        for p in $HTTP_PIDS; do kill "$p" 2>/dev/null || true; done
        HTTP_PIDS=""
    done
}

_netem_duplication() {
    local resp

    # duplication 需要隔离运行（不能与其他 netem 共存）
    tc qdisc del dev "$TAP_DEV" root 2>/dev/null || true
    tc qdisc add dev "$TAP_DEV" root handle 1: htb 2>/dev/null || true

    python3 -m http.server 8003 --directory "$SCRIPT_DIR" >/dev/null 2>&1 &
    HTTP_PIDS="$HTTP_PIDS $!"
    tc class add dev "$TAP_DEV" parent 1: classid 1:2 htb rate 100mbit 2>/dev/null || true
    tc filter add dev "$TAP_DEV" parent 1: protocol ip prio 1 u32 \
        flowid 1:2 match ip sport 8003 0xffff 2>/dev/null || true
    tc filter add dev "$TAP_DEV" parent 1: protocol ip prio 1 u32 \
        flowid 1:2 match ip dport 8003 0xffff 2>/dev/null || true
    tc qdisc add dev "$TAP_DEV" parent 1:2 netem duplicate 50% 2>/dev/null || true
    sleep 2

    resp=$(timeout 30 "$REPO_DIR/tools/level-ip" "$REPO_DIR/apps/curl/curl" \
        10.0.0.5 8003 2>/dev/null \
        | sed 's/^Date:.*/Date:/' | sed 's/^Server:.*/Server:/') || true
    if diff -q "$FIXTURE" <(echo "$resp") >/dev/null 2>&1; then
        pass "curl sync (duplication)"
    else
        fail "curl sync (duplication)"
    fi

    for p in $HTTP_PIDS; do kill "$p" 2>/dev/null || true; done
    HTTP_PIDS=""
    tc qdisc del dev "$TAP_DEV" root 2>/dev/null || true
}
