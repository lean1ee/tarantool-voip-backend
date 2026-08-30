# Real-Time Telecom Billing, Anti-Fraud & Live CDR Analytics with Tarantool

This document demonstrates the unique architectural advantage of utilizing **Tarantool 3.x** as a unified data and execution layer for **Kamailio**, **OpenSIPS**, and **RTPEngine**.

---

## 🏛️ Architecture Overview

In legacy VoIP deployments, state is fragmented across multiple independent data tiers:
1. **Redis**: Ephemeral SIP dialog and media session state.
2. **SQL Database (PostgreSQL / MySQL)**: Subscriber profiles, account balances, tariff matrices.
3. **Batch Log Aggregators (Filebeats / ClickHouse)**: Delayed post-call CDR processing and billing.

### 1. Call Setup & Authorization Flow

```mermaid
%%{init: {
  'theme': 'base',
  'themeVariables': {
    'fontFamily': 'Inter, system-ui, sans-serif',
    'fontSize': '13px',
    'darkMode': true,
    'actorBkg': '#1e293b',
    'actorBorder': '#3b82f6',
    'actorTextColor': '#f8fafc',
    'actorLineColor': '#64748b',
    'signalColor': '#60a5fa',
    'signalTextColor': '#f8fafc',
    'labelBoxBkgColor': '#0f172a',
    'labelBoxBorderColor': '#334155',
    'labelTextColor': '#f8fafc',
    'loopTextColor': '#f8fafc',
    'noteBorderColor': '#3b82f6',
    'noteBkgColor': '#1e293b',
    'noteTextColor': '#f8fafc',
    'activationBorderColor': '#3b82f6',
    'activationBkgColor': '#1d4ed833'
  }
}}%%
sequenceDiagram
    autonumber
    actor Caller as 📱 SIP Endpoint (Alice)
    participant Kam as ⚡ Kamailio / OpenSIPS
    participant TNT as 🔥 Tarantool 3.x (Memtx)
    participant RTPE as 🎙️ RTPEngine Media Relay

    Caller->>+Kam: SIP INVITE (To: +12025550143)
    
    rect rgba(59, 130, 246, 0.08)
        Kam->>+TNT: IProto: billing_authorize_call(caller, callee, call_id)
        Note over TNT: ⚡ In-Memory Lua Transaction (&lt; 0.2 ms):<br/>1. Check Balance in space `subscribers`<br/>2. Anti-fraud channel limit check<br/>3. Match prefix `1` in `tariffs` ($0.02/min)<br/>4. Store active dialog in `kam_dialogs`
        TNT-->>-Kam: { allowed: true, max_duration: 75000s, rate: 0.02 }
    end

    alt Balance & Concurrency Check Passed
        Kam->>+RTPE: NG offer (allocate RTP relay ports)
        RTPE-->>-Kam: 200 OK + SDP (Media Endpoints)
        Kam-->>Caller: 180 Ringing
        Kam-->>-Caller: 200 OK (Call Established)
    else Insufficient Funds or Channel Limit Exceeded
        Kam-->>Caller: 402 Payment Required / 486 Channel Limit
    end
```

### 2. Call Teardown, Rating & Instant CDR Finalization

```mermaid
%%{init: {
  'theme': 'base',
  'themeVariables': {
    'fontFamily': 'Inter, system-ui, sans-serif',
    'fontSize': '13px',
    'darkMode': true,
    'actorBkg': '#1e293b',
    'actorBorder': '#10b981',
    'actorTextColor': '#f8fafc',
    'actorLineColor': '#64748b',
    'signalColor': '#34d399',
    'signalTextColor': '#f8fafc',
    'labelBoxBkgColor': '#0f172a',
    'labelBoxBorderColor': '#334155',
    'labelTextColor': '#f8fafc',
    'noteBorderColor': '#10b981',
    'noteBkgColor': '#1e293b',
    'noteTextColor': '#f8fafc'
  }
}}%%
sequenceDiagram
    autonumber
    actor Caller as 📱 SIP Endpoint (Alice)
    participant Kam as ⚡ Kamailio / OpenSIPS
    participant RTPE as 🎙️ RTPEngine Media Relay
    participant TNT as 🔥 Tarantool 3.x (Memtx)
    participant BI as 📊 Real-Time Grafana / BI

    Caller->>+Kam: SIP BYE (Teardown Call)
    
    Kam->>+RTPE: NG delete (query audio quality counters)
    RTPE-->>-Kam: { duration: 185s, mos: 4.42, jitter: 1.15ms, loss: 0.02% }
    
    rect rgba(16, 185, 129, 0.08)
        Kam->>+TNT: IProto: billing_finalize_cdr(call_id, 185, stats)
        Note over TNT: 💾 Instant Teardown Transaction (&lt; 0.2 ms):<br/>1. Calculate exact cost: 4 mins @ $0.02 = $0.08<br/>2. Atomically debit balance: $25.00 -> $24.92<br/>3. Insert CDR enriched with MOS 4.42 & Jitter<br/>4. Evict active dialog from `kam_dialogs`
        TNT-->>-Kam: { status: "ok", billed: 0.08, remaining_balance: 24.92 }
    end
    
    Kam-->>-Caller: 200 OK
    
    Note over BI,TNT: Continuous Non-Blocking Polling (0 lock on SIP signaling)
    BI->>+TNT: IProto: billing_get_live_stats()
    TNT-->>-BI: { active_calls: 0, revenue: $0.08, fleet_mos: 4.42 }
```

---

## 💻 Kamailio KEMI Lua Integration Example

In `kamailio.lua`, call authorization and CDR finalization are invoked with native non-blocking RPCs:

```lua
-- Kamailio KEMI Routing Logic (kamailio.lua)

function ksr_route_invite()
    local caller = KSR.kx.get_from()
    local callee = KSR.kx.get_ruri_user()
    local call_id = KSR.kx.get_callid()

    -- 1. Atomic In-Memory Authorization & Anti-Fraud Check (< 0.2 ms)
    local res = KSR.tarantool.call("billing_authorize_call", {
        caller, callee, call_id, "rtpe-node-01"
    })

    if not res or not res.allowed then
        KSR.xlog.xwarn("Call rejected: " .. (res and res.reason or "AUTH_FAILED") .. "\n")
        if res and res.reason == "INSUFFICIENT_FUNDS" then
            KSR.sl.send_reply(402, "Payment Required")
        elseif res and res.reason == "MAX_CONCURRENT_CALLS_EXCEEDED" then
            KSR.sl.send_reply(486, "Busy Here - Channel Limit Exceeded")
        else
            KSR.sl.send_reply(403, "Forbidden")
        end
        return
    end

    -- Set max call duration timer in Kamailio dialog module
    KSR.dlg.set_timeout(res.max_duration_sec)
    KSR.xlog.xinfo("Authorized " .. caller .. " -> " .. callee .. " (rate: " .. res.rate_per_min .. "/min)\n")
    
    -- Engage RTPEngine Media Relay
    KSR.rtpengine.manage("replace-origin replace-session-connection")
    KSR.tm.t_relay()
end

function ksr_on_bye()
    local call_id = KSR.kx.get_callid()
    local duration = KSR.dlg.get_duration()

    -- 2. Query RTPEngine media statistics and finalize billing in Tarantool
    local stats = KSR.rtpengine.get_stats()
    
    KSR.tarantool.call("billing_finalize_cdr", {
        call_id,
        duration,
        stats.rx_packets or 0,
        stats.tx_packets or 0,
        stats.jitter_ms or 1.2,
        stats.loss_pct or 0.0,
        stats.mos or 4.4,
        "rtpe-node-01"
    })
end
```

---

## 📊 Real-Time Analytical Dashboard Queries

Because Tarantool supports concurrent read-only queries on secondary indexes and replicas without locking the active dialog table, real-time metrics can be polled continuously:

```lua
-- Stored procedure: billing_get_live_stats()
local stats = box.call('billing_get_live_stats')
-- Returns:
-- {
--   "active_calls": 1420,
--   "total_cdrs_processed": 85492,
--   "total_revenue": 4219.85,
--   "average_call_duration_sec": 194,
--   "average_fleet_mos": 4.38
-- }
```

---

## 🧪 Running the Demonstration Test

Execute the automated test suite against a running Tarantool instance:

```bash
python tests/test_realtime_billing_cdr.py
```

Output:
```text
==================================================================
  1. Setting up Carrier Tariffs and In-Memory Subscriber Profiles 
==================================================================
    [+] Tariffs configured: USA ($0.02/min), UK ($0.05/min), RU ($0.03/min)
    [+] Subscribers initialized: Alice ($25.00), Bob ($0.00), Charlie ($10.00, max 1 call)

==================================================================
  2. Testing Real-Time Call Authorization (SIP INVITE -> Tarantool)
==================================================================
    [+] Authorized Alice -> +12025550143 (USA): allowed=true, rate=$0.02/min, max_duration=75000s

==================================================================
  3. Testing Anti-Fraud: Insufficient Balance Rejection           
==================================================================
    [+] Rejected Bob ($0.00 balance): allowed=false, reason='INSUFFICIENT_FUNDS'

==================================================================
  4. Testing Anti-Fraud: Concurrent Call Limit Rejection          
==================================================================
    [+] Call 1 for Charlie: allowed=true
    [+] Call 2 for Charlie (limit=1): allowed=false, reason='MAX_CONCURRENT_CALLS_EXCEEDED'

==================================================================
  5. Testing Call Teardown: Balance Deduction & Quality CDR       
==================================================================
    [+] Call teardown processed in < 0.2 ms:
        - Duration: 185s (Billed: 4 minutes @ $0.02/min = $0.08)
        - RTPEngine Audio Quality: MOS 4.42, Jitter 1.15 ms, Loss 0.02%
        - Alice's Balance: $25.00 -> $24.92 (Atomically Deducted)
        - CDR generated: cdr-call-alice-001

==================================================================
  6. Testing Non-Blocking Real-Time Fleet Analytics & Reporting   
==================================================================
    [+] Live Analytics Query Executed (Zero impact on SIP signaling):
        - Total CDRs Processed: 1
        - Total Revenue: $0.08
        - Fleet Average MOS Score: 4.42
```
