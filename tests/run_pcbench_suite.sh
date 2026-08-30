#!/bin/bash
set -e

echo "Compiling unified pcbench with perfcached, Redis, and Tarantool support..."
gcc -O2 -std=gnu11 -I/perfcached/bench /perfcached/bench/pcbench.c -o /tmp/pcbench -lpthread
cd /perfcached
make perfcached
cp perfcached /tmp/perfcached

cat > /tmp/p.conf <<EOF
[daemon]
workers = 4
log_level = info
[memory]
arena_mb = 512
[secrets]
client = bench-client-secret
cluster = bench-cluster-secret
[listen]
tcp = 127.0.0.1:16488
plaintext = loopback
[collection b]
buckets_log2 = 17
EOF
chmod 640 /tmp/p.conf

/tmp/perfcached -f /tmp/p.conf > /tmp/p.log 2>&1 &
PID=$!
sleep 2

echo "=========================================================================================="
echo "          UNIFIED C-BENCHMARK (pcbench): PERFCACHED VS REDIS VS TARANTOOL                 "
echo "          Workload: 10,000 Keys, 64-Byte Values, Duration: 5s per test arm                "
echo "=========================================================================================="

echo ""
echo "--- 1. perfcached (4 Worker Threads, JSON Text Dialect) ---"
echo "[SET RTT d=1 c=4]"
/tmp/pcbench -h 127.0.0.1 -p 16488 -P perf -C b -c 4 -d 1 -M 0 -n 10000 -v 64 -T 5 -w 1
echo "[GET RTT d=1 c=4]"
/tmp/pcbench -h 127.0.0.1 -p 16488 -P perf -C b -c 4 -d 1 -M 100 -n 10000 -v 64 -T 5 -w 1
echo "[GET Pipelined d=32 c=8]"
/tmp/pcbench -h 127.0.0.1 -p 16488 -P perf -C b -c 8 -d 32 -M 100 -n 10000 -v 64 -T 5 -w 1
echo "[SET Pipelined d=32 c=8]"
/tmp/pcbench -h 127.0.0.1 -p 16488 -P perf -C b -c 8 -d 32 -M 0 -n 10000 -v 64 -T 5 -w 1

echo ""
echo "--- 2. Redis 8.10.1 (Single-Thread Core, RESP Dialect) ---"
echo "[SET RTT d=1 c=4]"
/tmp/pcbench -h redis_backend -p 6379 -P resp -c 4 -d 1 -M 0 -n 10000 -v 64 -T 5 -w 1
echo "[GET RTT d=1 c=4]"
/tmp/pcbench -h redis_backend -p 6379 -P resp -c 4 -d 1 -M 100 -n 10000 -v 64 -T 5 -w 1
echo "[GET Pipelined d=32 c=8]"
/tmp/pcbench -h redis_backend -p 6379 -P resp -c 8 -d 32 -M 100 -n 10000 -v 64 -T 5 -w 1
echo "[SET Pipelined d=32 c=8]"
/tmp/pcbench -h redis_backend -p 6379 -P resp -c 8 -d 32 -M 0 -n 10000 -v 64 -T 5 -w 1

echo ""
echo "--- 3. Tarantool 3.x (IProto Binary Protocol) ---"
echo "[SET RTT d=1 c=4]"
/tmp/pcbench -h tnt_backend -p 3301 -P tnt -c 4 -d 1 -M 0 -n 10000 -v 64 -T 5 -w 1
echo "[GET RTT d=1 c=4]"
/tmp/pcbench -h tnt_backend -p 3301 -P tnt -c 4 -d 1 -M 100 -n 10000 -v 64 -T 5 -w 1
echo "[GET Pipelined d=32 c=8]"
/tmp/pcbench -h tnt_backend -p 3301 -P tnt -c 8 -d 32 -M 100 -n 10000 -v 64 -T 5 -w 1
echo "[SET Pipelined d=32 c=8]"
/tmp/pcbench -h tnt_backend -p 3301 -P tnt -c 8 -d 32 -M 0 -n 10000 -v 64 -T 5 -w 1

kill -TERM $PID 2>/dev/null || true
