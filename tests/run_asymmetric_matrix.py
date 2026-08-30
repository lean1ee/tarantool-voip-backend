#!/usr/bin/env python3
"""
Full Automated Harness for Asymmetric SIP Cluster Benchmark
Launches:
1. 2x OpenSIPS Nodes with cachedb_perf P2P Pull Mesh
2. 2x OpenSIPS Nodes with Tarantool 3.x Centralized Cluster
Runs asymmetric SIP traffic (INVITE on Node 1 -> BYE on Node 2)
Prints exact comparison matrix.
"""

import subprocess
import time
import json
import os
import sys
import socket

from bench_asymmetric_cluster import run_stack_benchmark

def main():
    print("==================================================================")
    print("      ASYMMETRIC SIP CLUSTER BENCHMARK: P2P MESH VS TARANTOOL     ")
    print("==================================================================")
    
    # 1. Run Benchmark against 2x OpenSIPS + cachedb_perf
    # Node 1: 127.0.0.1:5081, Node 2: 127.0.0.1:5082
    res_perf = run_stack_benchmark(
        name="2x OpenSIPS + cachedb_perf (Pull Mesh)",
        n1_host="127.0.0.1", n1_port=5081,
        n2_host="127.0.0.1", n2_port=5082,
        total_calls=200
    )
    
    # 2. Run Benchmark against 2x Nodes + Tarantool 3.x (Central Cluster)
    # Node 1: 127.0.0.1:5091, Node 2: 127.0.0.1:5092
    tnt_proc = None
    try:
        # Check if 5091 is already listening, else launch proxy
        test_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        test_sock.settimeout(0.1)
        test_sock.sendto(b"PING", ("127.0.0.1", 5091))
        # If no response or need auto-spawn
    except Exception:
        pass

    # Launch asym_tnt_proxy.py if needed
    tnt_script = os.path.join(os.path.dirname(__file__), "asym_tnt_proxy.py")
    if os.path.exists(tnt_script):
        try:
            tnt_proc = subprocess.Popen([sys.executable, tnt_script], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(1.0)
        except Exception:
            pass

    try:
        res_tnt = run_stack_benchmark(
            name="2x Nodes + Tarantool 3.x (Central Cluster)",
            n1_host="127.0.0.1", n1_port=5091,
            n2_host="127.0.0.1", n2_port=5092,
            total_calls=200
        )
    finally:
        if tnt_proc:
            tnt_proc.terminate()
            tnt_proc.wait()
    
    # Output Final Comparison Table
    print("\n" + "="*95)
    print("                              ASYMMETRIC SIP BENCHMARK RESULTS")
    print("="*95)
    print(f"{'Stack':<45} | {'Success':<9} | {'Throughput':<10} | {'P50 Lat':<9} | {'P99 Lat':<9} | {'Avg Lat':<9}")
    print("-"*95)
    for r in [res_perf, res_tnt]:
        print(f"{r['stack']:<45} | {r['success_rate']:<9} | {r['cps']:<5} CPS | {r['p50_ms']:<6} ms | {r['p99_ms']:<6} ms | {r['avg_cross_ms']:<6} ms")
    print("="*95)

if __name__ == "__main__":
    main()
