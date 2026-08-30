# 🚀 Quick Start & Verification Guide

This guide allows anyone (reviewers, benchmarkers, and telecom engineers) to set up and verify the **Tarantool 3.x VoIP Backend** for **Kamailio**, **OpenSIPS**, and **RTPEngine** in **2 minutes**.

---

## ⚡ 1. Two-Minute Cluster Launch

### Prerequisites
* Docker & Docker Compose v2+
* Python 3.9+

### Start Cluster
Clone the repository and launch the full multi-stack environment:

```bash
# 1. Start all containers (Tarantool 3.x, Redis 8.x, Kamailio, OpenSIPS, RTPEngine)
docker compose up -d

# 2. Verify all containers are running healthy
docker compose ps
```

---

## 🧪 2. How to Verify Each Core Feature

### Feature 1: Real-Time Telecom Billing, Anti-Fraud & Live CDRs (Interactive Web UI)
Demonstrates replacing fragmented Redis + PostgreSQL + batch log collectors with **1 unified in-memory transactional database**.

```bash
# Launch the live visual dashboard
python examples/billing_demo_server.py
```
👉 **Open in Browser:** **[http://localhost:8089](http://localhost:8089)**

**What to test in the UI:**
1. Click **`📞 Alice Calls USA`**: Instant pre-call authorization in < 0.2 ms, credit reservation, and dialog insertion into `kam_dialogs`.
2. Click **`🚫 Bob Calls UK`**: Instant rejection with `402 Payment Required` (`INSUFFICIENT_FUNDS`) with zero state pollution.
3. Click **`⚠️ Charlie Calls`**: Anti-fraud test: 1st channel passes; 2nd concurrent channel is blocked (`MAX_CONCURRENT_CALLS_EXCEEDED`).
4. Click **`⏹️ Teardown Call`**: Atomically debits balance, deletes active dialog, and inserts rich CDR with RTPEngine media metrics (**MOS 4.42, Jitter 1.15 ms**).

---

### Feature 2: Full Unit & Integration Test Suite
Executes all 18 automated test suites across IProto framing, schema indexes, TTL background cleanup, billing logic, and Asterisk Realtime/Dialplan/CDR drivers:

```bash
python -m unittest discover -s tests
```
*Expected output:* `Ran 18 tests in ~0.18s -> OK`.

---

### Feature 3: End-to-End SIP Signaling & Media Relays (SIPp)
Executes live SIP calls through Kamailio 6.0 and OpenSIPS 3.5 backed by RTPEngine media sessions in Tarantool:

```bash
python tests/test_all_stacks.py
```
*Expected output:* `100/100 Successful SIP Dialogs (0 failed, 100% success rate)`.

---

### Feature 4: Live Matrix Benchmark (Tarantool vs Redis vs perfcached)
Runs the unified live performance benchmark across in-memory engines, UDP media signaling, and asymmetric SIP routing:

```bash
python tests/run_full_matrix_benchmark.py
```
*Outputs:* Complete latency distribution (P50, P90, P99), throughput OPS, and memory footprints saved to `benchmarks/matrix_benchmark_results.json`.

---

### Feature 5: Native C Benchmark Client (`pcbench`)
Benchmark Tarantool binary IProto directly against Redis RESP and `perfcached` using the unified C client:

```bash
# Compile pcbench
gcc -O3 -pthread -Ibenchmarks benchmarks/pcbench.c -o pcbench

# Benchmark Tarantool 3.x (Port 3301)
./pcbench -P tnt -h 127.0.0.1 -p 3301 -c 8 -d 32 -n 10000 -t 5

# Benchmark Redis 8.x (Port 6379)
./pcbench -P resp -h 127.0.0.1 -p 6379 -c 8 -d 32 -n 10000 -t 5
```

---

## 🏛️ Summary Matrix: Tarantool vs Alternatives

| Feature / Metric | Redis 8.x / Key-Value | P2P Mesh (Pull-on-Miss) | Tarantool 3.x VoIP Backend | Verification Command |
|:---|:---|:---|:---|:---|
| **Audio Jitter Spikes** | **18–20 ms** (BGSAVE COW) | Low | **ZERO (Streaming WAL)** | `python tests/run_full_matrix_benchmark.py` |
| **Node Failover Search** | $O(N)$ (Full DB scan) | Snapshot reload | **O(log N) (< 2 ms for 10k calls)** | `python tests/test_docker_cluster_ops.py` |
| **Pre-Call Rating & Billing** | External SQL (< 20 ms) | N/A | **< 0.2 ms (In-Memory Lua TX)** | `http://localhost:8089` |
| **SIP Call Teardown & CDR** | Delayed ETL batch logs | N/A | **Instant CDR with RTPEngine MOS** | `http://localhost:8089` |
| **RAM Footprint (10k calls)**| 10.82 MB | ~12.50 MB | **5.19 MB (-52% RAM)** | `benchmarks/index.html` |
| **Asymmetric SIP Throughput**| 850.0 CPS | 558.2 CPS | **1,003.4 CPS (+80% vs Mesh)** | `python tests/run_asymmetric_matrix.py` |

---

## 🛑 Stopping the Cluster

```bash
docker compose down
```
