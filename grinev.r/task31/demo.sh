#!/usr/bin/env bash
set -e

cd -- "$(dirname -- "$0")"
pause=${1:-600}
offset=${2:-300}
dir=$(mktemp -d /tmp/task31.XXXXXXXX)
socket=$dir/demo.sock
server_pid=
a_pid=
b_pid=

cleanup() {
    for pid in "$a_pid" "$b_pid" "$server_pid"; do
        if [[ -n $pid ]]; then
            kill -TERM "$pid" 2>/dev/null || :
            wait "$pid" 2>/dev/null || :
        fi
    done
    rmdir "$dir" 2>/dev/null || :
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# Отдельный путь для демонстрации, чтобы не мешать обычному серверу.
./server --demo "$socket" &
server_pid=$!
sleep 1
if [[ ! -S $socket ]]; then
    printf 'Демонстрационный сервер не запустился.\n' >&2
    exit 1
fi

./client --demo a "$socket" "$pause" 0 </dev/null &
a_pid=$!
./client --demo b "$socket" "$pause" "$offset" </dev/null &
b_pid=$!

wait "$a_pid"
a_pid=
wait "$b_pid"
b_pid=
wait "$server_pid"
server_pid=
