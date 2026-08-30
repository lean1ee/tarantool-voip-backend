#!/usr/bin/env python3
"""
Asymmetric SIP Cluster Benchmark:
Compares Cross-Node State Sharing between:
1. 2x OpenSIPS + cachedb_perf (P2P Pull-on-Miss Mesh)
2. 2x OpenSIPS + Tarantool 3.x (Centralized IProto Cluster)

Flow:
- Client sends SIP INVITE to Node 1 (stores dialog state)
- Client sends SIP BYE to Node 2 (forces cross-node state lookup)
- Node 2 resolves dialog state and returns SIP 200 OK
"""

import time
import socket
import select
import statistics
import json
import os
import sys

def send_sip_dialog(node1_host, node1_port, node2_host, node2_port, call_id):
    """
    Sends INVITE to Node 1, waits for 200 OK,
    then sends BYE to Node 2 and measures cross-node resolution latency.
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    sock.settimeout(0.5)
    src_port = sock.getsockname()[1]
    
    # 1. INVITE -> Node 1
    invite_msg = (
        f"INVITE sip:bob@127.0.0.1:5060 SIP/2.0\r\n"
        f"Via: SIP/2.0/UDP 127.0.0.1:{src_port};rport;branch=z9hG4bK-{call_id}-1\r\n"
        f"From: <sip:alice@example.com>;tag=tagA-{call_id}\r\n"
        f"To: <sip:bob@example.com>\r\n"
        f"Call-ID: {call_id}\r\n"
        f"CSeq: 1 INVITE\r\n"
        f"X-Store-Dialog: 1\r\n"
        f"Content-Length: 0\r\n\r\n"
    )
    
    t_start = time.perf_counter()
    sock.sendto(invite_msg.encode(), (node1_host, node1_port))
    
    try:
        data, _ = sock.recvfrom(4096)
        resp1 = data.decode(errors='ignore')
        if "200" not in resp1 and "180" not in resp1:
            sock.close()
            return False, 0.0, 0.0
    except Exception:
        sock.close()
        return False, 0.0, 0.0
        
    t_invite = (time.perf_counter() - t_start) * 1000.0 # ms

    # 2. BYE -> Node 2 (forces cross-node lookup)
    bye_msg = (
        f"BYE sip:bob@127.0.0.1:5060 SIP/2.0\r\n"
        f"Via: SIP/2.0/UDP 127.0.0.1:{src_port};rport;branch=z9hG4bK-{call_id}-2\r\n"
        f"From: <sip:alice@example.com>;tag=tagA-{call_id}\r\n"
        f"To: <sip:bob@example.com>;tag=tagB-{call_id}\r\n"
        f"Call-ID: {call_id}\r\n"
        f"CSeq: 2 BYE\r\n"
        f"X-Fetch-Dialog: 1\r\n"
        f"Content-Length: 0\r\n\r\n"
    )
    
    t_bye_start = time.perf_counter()
    sock.sendto(bye_msg.encode(), (node2_host, node2_port))
    
    try:
        data, _ = sock.recvfrom(4096)
        resp2 = data.decode(errors='ignore')
        t_bye = (time.perf_counter() - t_bye_start) * 1000.0 # ms
        sock.close()
        if "200" in resp2:
            return True, t_invite, t_bye
        return False, t_invite, t_bye
    except Exception:
        sock.close()
        return False, t_invite, 0.0

def run_stack_benchmark(name, n1_host, n1_port, n2_host, n2_port, total_calls=500):
    print(f"\n[*] Benchmarking Stack: {name} ({total_calls} asymmetric calls)...")
    success = 0
    invite_latencies = []
    cross_lookup_latencies = []
    
    t_benchmark_start = time.perf_counter()
    
    for i in range(total_calls):
        cid = f"asym-{name.lower().replace(' ', '-')}-{int(time.time())}-{i}"
        ok, t_inv, t_bye = send_sip_dialog(n1_host, n1_port, n2_host, n2_port, cid)
        if ok:
            success += 1
            invite_latencies.append(t_inv)
            cross_lookup_latencies.append(t_bye)
        else:
            # Short sleep on error to allow buffer drainage
            time.sleep(0.001)
            
    total_time = time.perf_counter() - t_benchmark_start
    cps = success / total_time if total_time > 0 else 0
    
    p50 = statistics.median(cross_lookup_latencies) if cross_lookup_latencies else 0
    p95 = statistics.quantiles(cross_lookup_latencies, n=20)[18] if len(cross_lookup_latencies) >= 20 else p50
    p99 = statistics.quantiles(cross_lookup_latencies, n=100)[98] if len(cross_lookup_latencies) >= 100 else p95
    avg_cross = statistics.mean(cross_lookup_latencies) if cross_lookup_latencies else 0
    
    print(f"    [+] Success Rate: {success}/{total_calls} ({success*100.0/total_calls:.1f}%)")
    print(f"    [+] Total Time: {total_time:.2f}s | Throughput: {cps:.1f} CPS")
    print(f"    [+] Cross-Node Lookup Latency (BYE on Node 2):")
    print(f"        P50: {p50:.3f} ms | P95: {p95:.3f} ms | P99: {p99:.3f} ms | Avg: {avg_cross:.3f} ms")
    
    return {
        "stack": name,
        "total_calls": total_calls,
        "success": success,
        "success_rate": f"{success*100.0/total_calls:.1f}%",
        "total_time_s": round(total_time, 2),
        "cps": round(cps, 1),
        "p50_ms": round(p50, 3),
        "p95_ms": round(p95, 3),
        "p99_ms": round(p99, 3),
        "avg_cross_ms": round(avg_cross, 3)
    }

if __name__ == "__main__":
    print("==================================================================")
    print("      ASYMMETRIC SIP CLUSTER BENCHMARK (CROSS-NODE SHARING)       ")
    print("==================================================================")
    
    # We will invoke the harness from Docker runner
