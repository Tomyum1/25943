#!/usr/bin/env bash
set -eu
cd -- "$(dirname -- "$0")"

dir=$(mktemp -d /tmp/socket-demo.XXXXXXXX)
sock=$dir/s
server_pid=
client1_pid=
client2_pid=

cleanup() {
    for pid in "$client1_pid" "$client2_pid" "$server_pid"; do
        if [[ -n $pid ]]; then
            kill -TERM "$pid" 2>/dev/null || :
            wait "$pid" 2>/dev/null || :
        fi
    done
    rmdir "$dir"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

# Оба клиента запускаются сразу. Сервер завершится после двух EOF.
./server --demo "$sock" &
server_pid=$!
./client1 "$sock" &
client1_pid=$!
./client2 "$sock" &
client2_pid=$!

wait "$client1_pid"
client1_pid=
wait "$client2_pid"
client2_pid=
wait "$server_pid"
server_pid=
