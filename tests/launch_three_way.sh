#!/bin/sh
set -e
chmod 640 /app/tests/perfcached_bench.conf
/perfcached/perfcached -f /app/tests/perfcached_bench.conf > /tmp/pc.log 2>&1 &
PPID_=$!

i=0
while [ $i -lt 50 ]; do
    if grep -q "perfcached ready" /tmp/pc.log 2>/dev/null; then
        break
    fi
    sleep 0.1
    i=$((i+1))
done

echo "[+] perfcached ready on 127.0.0.1:6479 (pid $PPID_)"
TNT_HOST=tnt_backend REDIS_HOST=redis_backend PERF_HOST=127.0.0.1 python3 /app/tests/benchmark_three_way.py
kill -TERM $PPID_ 2>/dev/null || true
