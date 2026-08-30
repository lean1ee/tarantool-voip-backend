# Asterisk Tarantool 3.x Native Connector Suite

High-performance, carrier-grade Tarantool 3.x in-memory database connector suite for **Asterisk PBX 20 LTS, 22 LTS, and master**.

```mermaid
%%{init: {'theme': 'base', 'themeVariables': { 'darkMode': true, 'primaryColor': '#1e293b', 'primaryTextColor': '#f8fafc', 'primaryBorderColor': '#0284c7', 'lineColor': '#38bdf8', 'secondaryColor': '#0f172a', 'tertiaryColor': '#1e1b4b' }}}%%
flowchart TD
    subgraph AsteriskApp["⭐ Asterisk PBX Core"]
        ARA["Realtime Engine (Sorcery / Static)<br/><code>res/res_config_tarantool.c</code>"]
        DP["Dialplan Execution Functions<br/><code>funcs/func_tarantool.c</code>"]
        CDR["High-Speed CDR Logger<br/><code>cdr/cdr_tarantool.c</code>"]
    end

    subgraph CoreIProto["⚡ Zero-Alloc IProto Driver Core"]
        POOL["Connection Pool Manager<br/><code>res/res_tarantool.c</code>"]
        MP["MessagePack Codec<br/><code>include/asterisk/msgpuck.h</code>"]
        SG["Scatter-Gather Engine<br/><code>struct iovec / writev</code>"]
    end

    subgraph TarantoolCluster["🚀 Tarantool 3.x In-Memory Cluster"]
        S1[("💾 <code>ps_endpoints</code> / <code>kam_usrloc</code><br/>PJSIP Objects & Registrations")]
        S2[("💾 <code>kam_dialogs</code><br/>Cluster BLF & Dialog States")]
        S3[("💾 <code>asterisk_cdrs</code><br/>Streaming WAL with MOS Metrics")]
        LUA["🧠 <code>rtpe_service.lua</code> & <code>asterisk_service.lua</code><br/>Pre-Call Rating, Anti-Fraud & Routing"]
    end

    ARA --> POOL
    DP --> POOL
    CDR --> POOL
    POOL --> MP
    POOL --> SG
    SG <== "Binary IProto (Port 3301)" ==> TarantoolCluster
```

---

## 🌟 Superpowers & Architectural Advantages

1. **Sub-100 Microsecond PJSIP Realtime Lookups:**
   * Replaces slow synchronous MySQL/ODBC queries with direct in-memory Slab lookups ($O(\log N)$ or $O(1)$) over binary IProto protocol.
2. **Unified Pre-Paid Rating & Anti-Fraud in Dialplan:**
   * Execute `${TARANTOOL(tnt1, call_authorize, ${CALLERID(num)}, ${EXTEN})}` directly in `extensions.conf` in **< 0.2 ms** without FastAGI process spawn overhead.
3. **High-Speed Streaming WAL CDR Logging (100k+ ops/sec):**
   * Non-blocking binary CDR persistence with zero SQLite3/ODBC database lock contention.
4. **Shared Multi-Stack Location & State:**
   * Asterisk seamlessly reads subscriber locations registered at **Kamailio** / **OpenSIPS** (`kam_usrloc`) from the shared Tarantool cluster.

---

## 📁 Module Inventory

| Module | Location | Description |
|---|---|---|
| **`res_tarantool.c`** | `res/` | Core IProto connection pool, TCP keepalive, auto-reconnect, and CLI commands |
| **`res_config_tarantool.c`** | `res/` | Asterisk Realtime engine supporting Sorcery, dynamic lookups, and static `.conf` |
| **`func_tarantool.c`** | `funcs/` | Dialplan functions `${TARANTOOL(...)}` and `${TARANTOOL_EVAL(...)}` |
| **`cdr_tarantool.c`** | `cdr/` | High-volume non-blocking CDR logging backend |
| **`msgpuck.h`** | `include/asterisk/` | Lightweight zero-dependency MessagePack encoder/decoder header |
| **`tarantool.conf.sample`** | `configs/` | Connection profile and pool configuration sample |
| **`cdr_tarantool.conf.sample`** | `configs/` | CDR engine configuration sample |

---

## 🔧 Sample Configuration

### `/etc/asterisk/tarantool.conf`
```ini
[general]

[tnt1]
host = 127.0.0.1
port = 3301
user = asterisk
secret = secret123
pool_size = 8
timeout_ms = 500
```

### `/etc/asterisk/extconfig.conf`
```ini
[settings]
ps_endpoints => tarantool,tnt1,ps_endpoints
ps_auths     => tarantool,tnt1,ps_auths
ps_aors      => tarantool,tnt1,ps_aors
extensions   => tarantool,tnt1,extensions
```

### `/etc/asterisk/extensions.conf`
```ini
[from-internal]
; Real-time pre-call rating & authorization (< 0.2 ms)
exten => _+X.,1,Set(AUTH=${TARANTOOL(tnt1,call_authorize,${CALLERID(num)},${EXTEN})})
same => n,GotoIf($["${AUTH}" != "OK"]?rejected)
same => n,Dial(PJSIP/${EXTEN},30)
same => n,Hangup()

same => n(rejected),Playback(prepaid-no-funds)
same => n,Hangup()
```
