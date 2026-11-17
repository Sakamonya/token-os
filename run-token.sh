#!/usr/bin/env bash
# 跑真正的 token 链路：裸机内核 <-帧-> 中转脚本 <-HTTPS-> 云端
#
#   bash run-token.sh [秒数]
#
set -u
cd "$(dirname "$0")"

SECS=${1:-60}
mkdir -p log
rm -f log/link.sock log/console.log log/relay.log

# 内核：控制台写文件，token 链路走 unix socket
qemu-system-aarch64 -M virt,gic-version=2 -cpu cortex-a72 -m 256M \
  -display none \
  -serial file:log/console.log \
  -chardev socket,id=lnk,path="$PWD/log/link.sock",server=on,wait=on \
  -serial chardev:lnk \
  -kernel kernel.elf > log/qemu.log 2>&1 &
QPID=$!

# 等 QEMU 把 socket 建好
for _ in $(seq 1 100); do
  [ -S log/link.sock ] && break
  sleep 0.1
done

# 中转：接上 socket，把帧翻成 HTTPS
node relay.js "$PWD/log/link.sock" > log/relay.log 2>&1 &
RPID=$!

echo "跑 ${SECS} 秒..."
sleep "$SECS"

kill "$RPID" 2>/dev/null
kill "$QPID" 2>/dev/null
sleep 0.3
kill -9 "$RPID" "$QPID" 2>/dev/null

echo
echo "=== 内核控制台（尾部） ==="
tail -n 25 log/console.log 2>/dev/null
echo
echo "=== 中转日志（尾部） ==="
tail -n 12 log/relay.log 2>/dev/null
