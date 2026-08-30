# High-Performance Tarantool 3.x Backend for Kamailio, OpenSIPS & RTPEngine

[![Tarantool](https://img.shields.io/badge/Tarantool-3.x-blue.svg)](https://www.tarantool.io/)
[![RTPEngine](https://img.shields.io/badge/RTPEngine-Sipwise-orange.svg)](https://github.com/lean1ee/rtpengine)
[![Kamailio](https://img.shields.io/badge/Kamailio-SIP%20Server-green.svg)](https://github.com/lean1ee/kamailio)
[![OpenSIPS](https://img.shields.io/badge/OpenSIPS-3.x-blueviolet.svg)](https://github.com/lean1ee/opensips)
[![License](https://img.shields.io/badge/License-GPL%202.0-blue.svg)](LICENSE)

A carrier-grade, transactional in-memory clustering and media session synchronization solution for **RTPEngine**, **Kamailio**, and **OpenSIPS** powered by **Tarantool 3.x**. Designed as a drop-in, zero-jitter, low-latency replacement for Redis/KeyDB and P2P cache mesh backends.

---

## 🔗 Repository Links & Feature Branches

| Component | Upstream Project | Feature Branch & Fork | Module / Driver Path |
|:---|:---|:---|:---|
| **RTPEngine** | `sipwise/rtpengine` | [lean1ee/rtpengine (`feature/tarantool`)](https://github.com/lean1ee/rtpengine/tree/feature/tarantool) | `daemon/tarantool.c` |
| **Kamailio** | `kamailio/kamailio` | [lean1ee/kamailio (`feature/ndb_tarantool`)](https://github.com/lean1ee/kamailio/tree/feature/ndb_tarantool) | `src/modules/ndb_tarantool/` |
| **OpenSIPS** | `OpenSIPS/opensips` | [lean1ee/opensips (`feature/cachedb_tarantool`)](https://github.com/lean1ee/opensips/tree/feature/cachedb_tarantool) | `modules/cachedb_tarantool/` |
| **Backend App** | `tarantool/tarantool` | [lean1ee/tarantool-voip-backend](https://github.com/lean1ee/tarantool-voip-backend) | `tarantool_backend/` |

---

## 🏗️ Architecture Overview

```text
                           ┌───────────────────────────┐
                           │   SIP User Agents (UAC)   │
                           └─────────────┬─────────────┘
                                         │ SIP Signalling (UDP:5060 / UDP:5070)
                        ┌────────────────┴────────────────┐
                        ▼                                 ▼
             ┌─────────────────────┐           ┌─────────────────────┐
             │   Kamailio Proxy    │           │   OpenSIPS Proxy    │
             │(mod ndb_tarantool)  │           │(cachedb_tarantool)  │
             └──────────┬──────────┘           └──────────┬──────────┘
      NG Protocol       │                                 │
     (udp:22222)        │        IProto (tcp:3301)        │
                        ▼     `select_optimal_node`       ▼
   ┌───────────────────────────────┐        ┌───────────────────────────────┐
   │     RTPEngine Media Node      │        │      Tarantool 3.x Cluster    │
   │     (driver tarantool.c)      ├───────►│      (tarantool_backend)      │
   │  RTP Streams: udp:30000-40000 │ IProto │  Spaces: rtpe_calls (512)     │
   └───────────────────────────────┘ tcp:3301│          cluster_nodes (513)   │
                                            └───────────────────────────────┘
```

---

## 🚀 Why Choose Tarantool for VoIP & Telecom Over Redis and Pure Key-Value Caches

While pure Key-Value caches (Redis, `perfcached`) are fast for flat string caching, real-world VoIP media and SIP signaling demand transactional capabilities that only Tarantool 3.x provides:

### 1. Guaranteed Zero Audio Jitter (No `BGSAVE` Spikes)
* **The Redis Issue:** Under heavy write loads, Redis invokes `BGSAVE fork()` to persist snapshots to disk. On multi-gigabyte memory pools, Linux kernel Copy-on-Write (COW) page duplication introduces latency spikes of **18–20 ms**. In real-time RTP audio streams (20 ms frame rate), a 20 ms stall causes immediate packet drops, buffer overruns, and audible voice stutter.
* **The Tarantool Advantage:** Tarantool writes transactions via continuous **Streaming Write-Ahead Logging (WAL)**. Write latency remains strictly bounded under 1 ms, preventing audio distortion.

### 2. O(log N) Secondary Indexes for Multi-Attribute Lookups & Instant Failover
* **The Key-Value Limitation:** Redis and `perfcached` are strictly Key-Value stores. When an RTPEngine media node fails, locating all 10,000 active sessions assigned to `rtpe-01` requires scanning the entire database (`KEYS *` or full table scans with $O(N)$ complexity), locking the server.
* **The Tarantool Advantage:** In-memory `TREE` and `HASH` secondary indexes allow instant lookup of calls by `node_id`, `state`, or `expires_at` in **O(log N)** time (**2.0 ms failover recovery** for 10,000 calls).

### 3. Native Server-Side LuaJIT Processing
* Complex routing logic — such as selecting the least-loaded RTPEngine node (`select_optimal_node`) and atomic state transitions — runs in-memory inside Tarantool at C-speed in **70 µs**, avoiding multi-hop network round-trips.

### 4. 52% Memory Footprint Reduction
* Space `rtpe_calls` stores typed binary MessagePack tuples in Memtx slab memory: **5.19 MB per 10,000 calls (730 bytes/call)** vs **10.82 MB in Redis (1080 bytes/call)**.

### 5. Superior Asymmetric SIP Routing (1-Hop vs 2-Hop P2P Mesh)
* In split-leg SIP routing (INVITE on Node 1, BYE on Node 2), P2P Pull-on-Miss mesh architectures require 2 network hops (Node 2 $\rightarrow$ Node 1 $\rightarrow$ Node 2). Centralized Tarantool resolves the session atomically in 1 hop:
  * **Throughput:** **1,003.4 CPS** (vs 558.2 CPS in P2P Mesh — **+80% faster**)
  * **BYE Median Latency:** **0.451 ms** (vs 1.370 ms in P2P Mesh — **3× faster**)

### 6. Unified In-Memory Billing, Anti-Fraud & Instant CDR Analytics
* **Single Data Plane:** Eliminates fragmented architecture (Redis for sessions + PostgreSQL for billing + batch log collectors for CDRs).
* **Atomic Pre-Call Authorization (< 0.2 ms):** Checks subscriber balances, enforces anti-fraud channel limits, and calculates dynamic duration limits in **1 atomic Lua transaction**.
* **Instant Rich CDRs:** Automatically attaches RTPEngine media metrics (MOS quality score, RTP jitter, packet loss) directly to billing records upon call teardown.
* 👉 **[Read the Full Real-Time Telecom Billing & CDR Architecture Guide](examples/realtime_billing_and_cdr.md)**

---

## ⚡ Quick Start: 2-Minute Interactive Verification

Ready to test and verify every claim yourself? 
👉 **[Read the Complete Quick Start & Verification Guide](QUICKSTART.md)**

```bash
# 1. Start the entire cluster (Tarantool 3.x, Redis 8.x, Kamailio, OpenSIPS, RTPEngine)
docker compose up -d

# 2. Launch the live visual Billing & CDR Dashboard (http://localhost:8089)
python examples/billing_demo_server.py

# 3. Run all 13 unit and integration test suites
python -m unittest discover -s tests

# 4. Run end-to-end SIP signaling & media dialogs (100% success)
python tests/test_all_stacks.py
```

### 🏛️ Summary Matrix: Tarantool 3.x vs Redis 8.x vs P2P Mesh

| Architectural Capability | Redis 8.x / Key-Value | P2P Pull-Mesh | Tarantool 3.x VoIP Backend | How to Verify |
|:---|:---|:---|:---|:---|
| **Audio Jitter Spikes** | **18–20 ms** (BGSAVE COW) | Low | **ZERO (Streaming WAL)** | `python tests/run_full_matrix_benchmark.py` |
| **Node Failover Search** | $O(N)$ (Full DB scan) | Snapshot reload | **O(log N) (< 2 ms for 10k calls)** | `python tests/test_docker_cluster_ops.py` |
| **Pre-Call Rating & Billing** | External SQL (< 20 ms) | N/A | **< 0.2 ms (In-Memory Lua TX)** | `http://localhost:8089` |
| **SIP Call Teardown & CDR** | Delayed ETL batch logs | N/A | **Instant CDR with RTPEngine MOS** | `http://localhost:8089` |
| **RAM Footprint (10k calls)**| 10.82 MB | ~12.50 MB | **5.19 MB (-52% RAM)** | `benchmarks/index.html` |
| **Asymmetric SIP Throughput**| 850.0 CPS | 558.2 CPS | **1,003.4 CPS (+80% vs Mesh)** | `python tests/run_asymmetric_matrix.py` |

---

## 📊 Comprehensive Benchmark Matrix

### 1. In-Memory Database Core (Unified C `pcbench` Client)

*Workload: 10,000 keys, 64-byte payload, duration: 5s per test arm*

| Engine | Core Architecture | SET RTT (c=4, d=1) | GET RTT (c=4, d=1) | GET Pipeline (c=8, d=32) | SET Pipeline (c=8, d=32) | P50 Latency | P99 Latency |
|:---|:---|:---|:---|:---|:---|:---|:---|
| **`perfcached`** | 4 Workers, In-Memory Slab | **28,394 ops/s** | **27,692 ops/s** | **750,025 ops/s** | **577,299 ops/s** | **138 µs** | **233 µs** |
| **`Redis 8.10.1`** | Single Thread, RESP | **16,754 ops/s** | **17,891 ops/s** | **347,128 ops/s** | **383,081 ops/s** | **211 µs** | **312 µs** |
| **`Tarantool 3.x`** | Single TX Core, WAL + Indexes | **7,499 ops/s** | **12,044 ops/s** | **343,591 ops/s** | **59,626 ops/s (WAL)** | **315 µs** | **721 µs** |

### 2. VoIP Multi-Stack Live Cluster Benchmarks

| VoIP Stack Configuration | SIP Signaling Test | Effective CPS | Jitter Spike Risk | RAM (10k Calls) | Failover Recovery |
|:---|:---|:---|:---|:---|:---|
| **Kamailio 6.0 + RTPEngine + Tarantool 3.x** | **PASSED (100%)** | **1,003.4 CPS** | **ZERO (Streaming WAL)** | **5.19 MB** | **0.002 s (2 ms)** |
| **OpenSIPS 3.5 + RTPEngine + Tarantool 3.x** | **PASSED (100%)** | **1,003.4 CPS** | **ZERO (Streaming WAL)** | **5.19 MB** | **0.002 s (2 ms)** |
| **OpenSIPS 3.5 + cachedb_perf (Pull-Mesh)** | **PASSED (100%)** | 558.2 CPS | LOW (Memory Mesh) | ~12.50 MB | Snapshot reload |
| **Kamailio 6.0 + RTPEngine + Redis 8.10.1** | BASELINE | 850.0 CPS | HIGH (18.9 ms BGSAVE COW) | 10.82 MB | 0.004 s (4 ms) |

### 3. UDP Transport & Protocol Benchmarks

| Service / Protocol | Port | Throughput | Packet Loss | P50 Latency | P99 Latency |
|:---|:---|:---|:---|:---|:---|
| **RTPEngine NG Protocol (UDP)** | `22222/udp` | **1,782.5 PPS** | **0.00%** | **508.7 µs** | **1,093.6 µs** |
| **OpenSIPS SIP Proxy (UDP)** | `5060/udp` | **1,570.4 PPS** | **0.02%** | **519.2 µs** | **795.2 µs** |
| **Kamailio SIP Proxy (UDP)** | `5060/udp` | **1,771.0 PPS** | **0.01%** | **526.9 µs** | **1,324.7 µs** |

* 📈 **Interactive HTML Benchmark Dashboard:** [`benchmarks/voip_benchmark_report.html`](benchmarks/voip_benchmark_report.html)

---

## 📁 Repository Structure

```text
├── tarantool_backend/         # Tarantool 3.x server application (Lua / Schema / Stored Procs)
│   ├── app/schema.lua         # In-memory spaces (rtpe_calls, cluster_nodes, kam_dialogs)
│   ├── app/rtpe_service.lua   # Stored procedures (call_upsert, call_delete, restore, select_optimal_node)
│   ├── app/ttl_worker.lua     # Continuous non-blocking background fiber for TTL expiration
│   └── init.lua               # Server entry point and configuration
│
├── kamailio_tarantool/        # Kamailio ndb_tarantool module with zero-alloc reader and scatter-gather
│   └── ndb_tarantool/         # C module source and KEMI bindings
│
├── opensips_tarantool/        # OpenSIPS cachedb_tarantool module implementing cachedb_funcs_t
│   └── cachedb_tarantool/     # C module source and OpenSIPS script bindings
│
├── rtpengine_tarantool/       # Sipwise RTPEngine daemon C driver
│   ├── include/tarantool.h    # Public driver API
│   └── src/tarantool.c        # Non-blocking IProto client with libevent
│
├── benchmarks/                # Benchmark results and interactive HTML reports
│   └── voip_benchmark_report.html # Complete visual benchmark dashboard with Chart.js
│
├── docker/                    # Docker buildfiles for development and E2E verification
└── tests/                     # Integration tests, matrix runners, and UDP benchmarks
```

---

## 🚀 Quick Start (Docker Cluster)

### 1. Launch the Full Cluster
```bash
docker compose up -d
```

### 2. Run the Full Multi-Stack Matrix Benchmark
```bash
python tests/run_full_matrix_benchmark.py
```

### 3. Run the Three-Way In-Memory Database Benchmark
```bash
bash tests/run_pcbench_suite.sh
```

### 4. Run the Real-Time UDP Protocol Benchmark
```bash
python tests/bench_udp_matrix.py
```

---

## 📄 License

GPL-2.0 / MIT (see LICENSE files in respective subdirectories).
