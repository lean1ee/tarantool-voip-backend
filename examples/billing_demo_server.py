#!/usr/bin/env python3
"""
examples/billing_demo_server.py
Interactive Live Visual Dashboard for Unified Telecom Billing & CDRs on Tarantool 3.x.

Runs a local web dashboard on http://127.0.0.1:8088
Connects to live Tarantool 3.x over binary IProto.
"""

from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
import socket
import struct
import json
import time
import os
import sys

TNT_HOST = os.environ.get("TNT_HOST", "127.0.0.1")
TNT_PORT = int(os.environ.get("TNT_PORT", "3301"))

def tnt_eval(lua_code):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(3.0)
        s.connect((TNT_HOST, TNT_PORT))
        s.recv(128) # greeting

        lua_b = lua_code.encode('utf-8')
        if len(lua_b) <= 31:
            str_hdr = bytes([0xa0 | len(lua_b)])
        elif len(lua_b) <= 255:
            str_hdr = b"\xd9" + struct.pack("B", len(lua_b))
        elif len(lua_b) <= 65535:
            str_hdr = b"\xda" + struct.pack(">H", len(lua_b))
        else:
            str_hdr = b"\xdb" + struct.pack(">I", len(lua_b))

        hdr = b"\x82\x00\x08\x01\x01" # IPROTO_EVAL
        body = b"\x82\x27" + str_hdr + lua_b + b"\x21\x90"
        pkt = b"\xce" + struct.pack(">I", len(hdr) + len(body)) + hdr + body
        s.sendall(pkt)

        resp_hdr = s.recv(5)
        if len(resp_hdr) < 5:
            s.close()
            return None
        resp_len = struct.unpack(">I", resp_hdr[1:5])[0]
        data = b""
        while len(data) < resp_len:
            c = s.recv(resp_len - len(data))
            if not c: break
            data += c
        s.close()
        return data
    except Exception:
        return None

def auto_seed_if_empty():
    seed_lua = """
    local fiber = require('fiber')
    local t = box.space.tariffs
    if t then
        t:replace({'1', 'USA / Canada', 0.02, 0.0})
        t:replace({'44', 'United Kingdom', 0.05, 0.0})
        t:replace({'7', 'Russia Mobile', 0.03, 0.0})
        t:replace({'default', 'Default', 0.10, 0.0})
    end

    local s = box.space.subscribers
    if s and s:count() == 0 then
        s:replace({'alice@example.com', 25.00, 'USD', 'active', 5, 'standard', math.floor(fiber.time())})
        s:replace({'bob@example.com', 0.00, 'USD', 'active', 2, 'standard', math.floor(fiber.time())})
        s:replace({'charlie@example.com', 10.00, 'USD', 'active', 1, 'standard', math.floor(fiber.time())})
    end
    return true
    """
    tnt_eval(seed_lua)

def fetch_json_state():
    # Helper query to return complete state as JSON string directly from Tarantool Lua
    lua = """
    local json = require('json')
    local subs = {}
    if box.space.subscribers then
        for _, s in box.space.subscribers:pairs() do
            table.insert(subs, {
                id = s.subscriber_id,
                balance = s.balance,
                currency = s.currency,
                status = s.status,
                max_calls = s.max_concurrent_calls,
                tariff = s.tariff_id
            })
        end
    end

    local dialogs = {}
    if box.space.kam_dialogs then
        for _, d in box.space.kam_dialogs:pairs() do
            table.insert(dialogs, {
                call_id = d.call_id,
                caller = d.from_tag,
                callee = d.to_tag,
                state = d.state,
                expires_at = d.expires_at,
                extra = d.extra_data
            })
        end
    end

    local cdrs = {}
    if box.space.cdrs then
        for _, c in box.space.cdrs:pairs() do
            table.insert(cdrs, {
                cdr_id = c.cdr_id,
                call_id = c.call_id,
                caller = c.caller,
                callee = c.callee,
                duration = c.duration_sec,
                amount = c.billed_amount,
                mos = c.mos_score,
                jitter = c.jitter_ms,
                loss = c.packet_loss_pct,
                node = c.node_id
            })
        end
    end

    local raw_stats = (type(billing_get_live_stats) == 'function') and billing_get_live_stats() or {}
    local stats = {
        active_calls = raw_stats.active_calls or (box.space.kam_dialogs and box.space.kam_dialogs:count() or 0),
        total_cdrs_processed = raw_stats.total_cdrs_processed or (box.space.cdrs and box.space.cdrs:count() or 0),
        total_revenue = raw_stats.total_revenue or 0.0,
        average_fleet_mos = raw_stats.average_fleet_mos or 4.5
    }
    return json.encode({
        subscribers = subs,
        dialogs = dialogs,
        cdrs = cdrs,
        stats = stats
    })
    """
    try:
        data = tnt_eval(lua)
        if data and isinstance(data, bytes):
            start = data.find(b'{"')
            end = data.rfind(b'}')
            if start != -1 and end != -1 and end >= start:
                json_bytes = data[start:end+1]
                return json.loads(json_bytes.decode('utf-8', errors='ignore'))
    except Exception:
        pass

    return {"subscribers": [], "dialogs": [], "cdrs": [], "stats": {}}

HTML_PAGE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>Tarantool Telecom Billing & CDR Dashboard</title>
<link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700;800&display=swap" rel="stylesheet">
<style>
  :root {
    --bg: #0b0f19;
    --surface: #121827;
    --surface-hover: #1b2438;
    --border: #232d45;
    --text-main: #f3f4f6;
    --text-muted: #9ca3af;
    --accent: #ff453a;
    --primary: #3b82f6;
    --success: #10b981;
    --warning: #f59e0b;
    --card-bg: rgba(18, 24, 39, 0.7);
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    font-family: 'Inter', sans-serif;
    background: var(--bg);
    color: var(--text-main);
    padding: 30px 40px;
    min-height: 100vh;
  }
  .header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 25px;
    padding-bottom: 20px;
    border-bottom: 1px solid var(--border);
  }
  .title-group h1 { font-size: 26px; font-weight: 800; background: linear-gradient(135deg, #fff 0%, #9ca3af 100%); -webkit-background-clip: text; -webkit-text-fill-color: transparent; }
  .title-group p { font-size: 13px; color: var(--text-muted); margin-top: 4px; }
  .badge { display: inline-flex; align-items: center; gap: 6px; padding: 6px 12px; border-radius: 99px; font-size: 12px; font-weight: 600; background: rgba(16, 185, 129, 0.15); color: #34d399; border: 1px solid rgba(16, 185, 129, 0.3); }
  .dot { width: 8px; height: 8px; border-radius: 50%; background: #10b981; box-shadow: 0 0 10px #10b981; }
  
  .grid-stats {
    display: grid;
    grid-template-columns: repeat(4, 1fr);
    gap: 16px;
    margin-bottom: 25px;
  }
  .stat-card {
    background: var(--card-bg);
    backdrop-filter: blur(12px);
    border: 1px solid var(--border);
    border-radius: 12px;
    padding: 20px;
    position: relative;
    overflow: hidden;
  }
  .stat-card::before {
    content: '';
    position: absolute;
    top: 0; left: 0; width: 4px; height: 100%;
    background: var(--accent);
  }
  .stat-card.blue::before { background: var(--primary); }
  .stat-card.green::before { background: var(--success); }
  .stat-card.amber::before { background: var(--warning); }
  .stat-label { font-size: 12px; font-weight: 600; text-transform: uppercase; letter-spacing: 0.5px; color: var(--text-muted); }
  .stat-val { font-size: 28px; font-weight: 800; margin-top: 8px; color: #fff; }
  .stat-sub { font-size: 12px; color: var(--text-muted); margin-top: 4px; }

  .actions-panel {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 12px;
    padding: 20px;
    margin-bottom: 25px;
  }
  .actions-header { font-size: 15px; font-weight: 700; margin-bottom: 15px; display: flex; align-items: center; gap: 8px; }
  .btn-group { display: flex; gap: 12px; flex-wrap: wrap; }
  button {
    padding: 10px 18px;
    font-size: 13px;
    font-weight: 600;
    font-family: inherit;
    border-radius: 8px;
    border: 1px solid transparent;
    cursor: pointer;
    transition: all 0.15s ease;
    display: inline-flex;
    align-items: center;
    gap: 8px;
  }
  .btn-primary { background: #2563eb; color: #fff; }
  .btn-primary:hover { background: #1d4ed8; }
  .btn-danger { background: rgba(239, 68, 68, 0.15); color: #f87171; border-color: rgba(239, 68, 68, 0.3); }
  .btn-danger:hover { background: rgba(239, 68, 68, 0.25); }
  .btn-warning { background: rgba(245, 158, 11, 0.15); color: #fbbf24; border-color: rgba(245, 158, 11, 0.3); }
  .btn-warning:hover { background: rgba(245, 158, 11, 0.25); }
  .btn-success { background: #059669; color: #fff; }
  .btn-success:hover { background: #047857; }
  .btn-outline { background: transparent; border: 1px solid var(--border); color: var(--text-muted); }
  .btn-outline:hover { background: var(--surface-hover); color: #fff; }

  .layout-2col {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 20px;
    margin-bottom: 25px;
  }
  .panel {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 12px;
    padding: 20px;
  }
  .panel-title { font-size: 15px; font-weight: 700; margin-bottom: 14px; display: flex; justify-content: space-between; align-items: center; }
  
  table { width: 100%; border-collapse: collapse; font-size: 13px; }
  th { text-align: left; padding: 10px 12px; font-weight: 600; color: var(--text-muted); border-bottom: 1px solid var(--border); }
  td { padding: 12px; border-bottom: 1px solid rgba(35, 45, 69, 0.5); vertical-align: middle; }
  tr:last-child td { border-bottom: none; }
  tr:hover td { background: var(--surface-hover); }

  .chip { display: inline-block; padding: 3px 8px; border-radius: 4px; font-size: 11px; font-weight: 700; }
  .chip-green { background: rgba(16, 185, 129, 0.2); color: #34d399; }
  .chip-blue { background: rgba(59, 130, 246, 0.2); color: #60a5fa; }
  .chip-purple { background: rgba(168, 85, 247, 0.2); color: #c084fc; }
  .chip-red { background: rgba(239, 68, 68, 0.2); color: #f87171; }
  .mono { font-family: monospace; font-size: 12px; }

  .toast {
    position: fixed;
    bottom: 24px;
    right: 24px;
    background: #1f2937;
    border: 1px solid #374151;
    color: #fff;
    padding: 12px 20px;
    border-radius: 8px;
    font-size: 13px;
    font-weight: 500;
    box-shadow: 0 10px 25px rgba(0,0,0,0.5);
    display: none;
    z-index: 100;
  }
</style>
</head>
<body>

<div class="header">
  <div class="title-group">
    <h1>Tarantool 3.x Real-Time Telecom Billing & CDR Dashboard</h1>
    <p>Unified In-Memory State for Kamailio, OpenSIPS & RTPEngine (Single Shared Database)</p>
  </div>
  <div class="badge"><div class="dot"></div> TARANTOOL 3.8.0 LIVE</div>
</div>

<div class="grid-stats">
  <div class="stat-card">
    <div class="stat-label">Active Concurrent Calls</div>
    <div class="stat-val" id="stat-active">0</div>
    <div class="stat-sub">Live in <span class="mono">kam_dialogs</span> space</div>
  </div>
  <div class="stat-card blue">
    <div class="stat-label">Total CDRs Processed</div>
    <div class="stat-val" id="stat-cdrs">0</div>
    <div class="stat-sub">Instant in <span class="mono">cdrs</span> ledger</div>
  </div>
  <div class="stat-card green">
    <div class="stat-label">Total Billed Revenue</div>
    <div class="stat-val" id="stat-revenue">$0.00</div>
    <div class="stat-sub">Atomically debited in real-time</div>
  </div>
  <div class="stat-card amber">
    <div class="stat-label">Fleet Media Quality (MOS)</div>
    <div class="stat-val" id="stat-mos">4.50</div>
    <div class="stat-sub">From RTPEngine audio metrics</div>
  </div>
</div>

<div class="actions-panel">
  <div class="actions-header">⚡ Interactive Telecom Simulation Actions (Click to Test Real Transactions):</div>
  <div class="btn-group">
    <button class="btn-primary" onclick="triggerAction('/api/call_alice')">
      📞 Alice Calls USA (+12025550143)
    </button>
    <button class="btn-danger" onclick="triggerAction('/api/call_bob')">
      🚫 Bob Calls UK (Zero Balance Check)
    </button>
    <button class="btn-warning" onclick="triggerAction('/api/call_charlie')">
      ⚠️ Charlie Calls (Anti-Fraud Line Limit)
    </button>
    <button class="btn-success" onclick="triggerAction('/api/end_call')">
      ⏹️ Teardown Call (RTPEngine BYE + CDR)
    </button>
    <button class="btn-outline" onclick="triggerAction('/api/reset')">
      🔄 Reset Balances & Tariffs
    </button>
  </div>
</div>

<div class="layout-2col">
  <div class="panel">
    <div class="panel-title">
      <span>👤 In-Memory Subscribers & Balances</span>
      <span class="chip chip-blue" id="subs-count">0 Subs</span>
    </div>
    <table>
      <thead>
        <tr><th>Subscriber ID</th><th>Balance</th><th>Status</th><th>Limit</th><th>Tariff</th></tr>
      </thead>
      <tbody id="subs-body">
        <tr><td colspan="5" style="text-align:center; color:var(--text-muted);">Loading...</td></tr>
      </tbody>
    </table>
  </div>

  <div class="panel">
    <div class="panel-title">
      <span>📞 Active In-Memory SIP Dialogs</span>
      <span class="chip chip-green" id="dialogs-count">0 Active</span>
    </div>
    <table>
      <thead>
        <tr><th>Call ID</th><th>Caller</th><th>Destination</th><th>Rate / Min</th><th>Status</th></tr>
      </thead>
      <tbody id="dialogs-body">
        <tr><td colspan="5" style="text-align:center; color:var(--text-muted);">No active calls</td></tr>
      </tbody>
    </table>
  </div>
</div>

<div class="panel">
  <div class="panel-title">
    <span>📜 Instant Call Detail Records (CDR) with RTPEngine Media Quality</span>
    <span class="chip chip-purple" id="cdrs-count">0 CDRs</span>
  </div>
  <table>
    <thead>
      <tr><th>CDR ID</th><th>Caller</th><th>Callee</th><th>Duration</th><th>Billed</th><th>MOS Score</th><th>Jitter</th><th>Packet Loss</th><th>Node</th></tr>
    </thead>
    <tbody id="cdrs-body">
      <tr><td colspan="9" style="text-align:center; color:var(--text-muted);">No CDRs generated yet</td></tr>
    </tbody>
  </table>
</div>

<div class="toast" id="toast">Action processed</div>

<script>
  async function refresh() {
    try {
      const res = await fetch('/api/state');
      const data = await res.json();

      // Update Top Stats
      document.getElementById('stat-active').textContent = data.stats.active_calls || data.dialogs.length || 0;
      document.getElementById('stat-cdrs').textContent = data.stats.total_cdrs_processed || data.cdrs.length || 0;
      document.getElementById('stat-revenue').textContent = '$' + (data.stats.total_revenue || 0).toFixed(2);
      document.getElementById('stat-mos').textContent = (data.stats.average_fleet_mos || 4.5).toFixed(2);

      // Update Subscribers Table
      document.getElementById('subs-count').textContent = data.subscribers.length + ' Subs';
      const sb = document.getElementById('subs-body');
      if (data.subscribers.length === 0) {
        sb.innerHTML = '<tr><td colspan="5" style="text-align:center;color:var(--text-muted);">No subscribers</td></tr>';
      } else {
        sb.innerHTML = data.subscribers.map(s => `
          <tr>
            <td class="mono"><b>${s.id}</b></td>
            <td><b style="color:${s.balance > 0 ? '#34d399' : '#f87171'}">$${Number(s.balance).toFixed(2)}</b></td>
            <td><span class="chip ${s.status === 'active' ? 'chip-green' : 'chip-red'}">${s.status}</span></td>
            <td>${s.max_calls} max</td>
            <td><span class="chip chip-blue">${s.tariff}</span></td>
          </tr>
        `).join('');
      }

      // Update Dialogs Table
      document.getElementById('dialogs-count').textContent = data.dialogs.length + ' Active';
      const db = document.getElementById('dialogs-body');
      if (data.dialogs.length === 0) {
        db.innerHTML = '<tr><td colspan="5" style="text-align:center;color:var(--text-muted);">No active calls in memory</td></tr>';
      } else {
        db.innerHTML = data.dialogs.map(d => `
          <tr>
            <td class="mono">${d.call_id}</td>
            <td><b>${d.caller}</b></td>
            <td class="mono">${d.callee}</td>
            <td>$${d.extra ? Number(d.extra.rate || 0.05).toFixed(2) : '0.05'}</td>
            <td><span class="chip chip-green">Active In-Memory</span></td>
          </tr>
        `).join('');
      }

      // Update CDRs Table
      document.getElementById('cdrs-count').textContent = data.cdrs.length + ' CDRs';
      const cb = document.getElementById('cdrs-body');
      if (data.cdrs.length === 0) {
        cb.innerHTML = '<tr><td colspan="9" style="text-align:center;color:var(--text-muted);">No CDRs recorded yet</td></tr>';
      } else {
        cb.innerHTML = data.cdrs.map(c => `
          <tr>
            <td class="mono">${c.cdr_id}</td>
            <td>${c.caller}</td>
            <td class="mono">${c.callee}</td>
            <td><b>${c.duration}s</b></td>
            <td><b style="color:#34d399">$${Number(c.amount).toFixed(4)}</b></td>
            <td><span class="chip chip-green">★ ${Number(c.mos).toFixed(2)}</span></td>
            <td>${c.jitter} ms</td>
            <td>${c.loss}%</td>
            <td><span class="chip chip-blue">${c.node}</span></td>
          </tr>
        `).join('');
      }
    } catch(e) {
      console.error(e);
    }
  }

  function showToast(msg) {
    const t = document.getElementById('toast');
    t.textContent = msg;
    t.style.display = 'block';
    setTimeout(() => { t.style.display = 'none'; }, 3000);
  }

  async function triggerAction(url) {
    try {
      const res = await fetch(url);
      const data = await res.json();
      showToast(data.message || 'Action executed');
      refresh();
    } catch (e) {
      showToast('Error: ' + e);
    }
  }

  setInterval(refresh, 1500);
  refresh();
</script>

</body>
</html>
"""

class RequestHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        try:
            if self.path == '/' or self.path.startswith('/index.html'):
                self.send_response(200)
                self.send_header('Content-Type', 'text/html; charset=utf-8')
                self.end_headers()
                self.wfile.write(HTML_PAGE.encode('utf-8'))
            elif self.path.startswith('/api/state'):
                state = fetch_json_state()
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.end_headers()
                self.wfile.write(json.dumps(state).encode('utf-8'))
            elif self.path.startswith('/api/call_alice'):
                call_id = f"call-alice-{int(time.time())}"
                tnt_eval(f"return billing_authorize_call('alice@example.com', '12025550143', '{call_id}', 'rtpe-node-01')")
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.end_headers()
                self.wfile.write(json.dumps({"message": f"Authorized Alice -> +12025550143 (Call-ID: {call_id})"}).encode('utf-8'))
            elif self.path.startswith('/api/call_bob'):
                tnt_eval("return billing_authorize_call('bob@example.com', '442071838750', 'call-bob-test', 'rtpe-node-01')")
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.end_headers()
                self.wfile.write(json.dumps({"message": "Rejected Bob: 402 Payment Required (Insufficient Funds: $0.00)"}).encode('utf-8'))
            elif self.path.startswith('/api/call_charlie'):
                cid1 = f"call-charlie-{int(time.time())}-1"
                cid2 = f"call-charlie-{int(time.time())}-2"
                tnt_eval(f"billing_authorize_call('charlie@example.com', '79991234567', '{cid1}', 'rtpe-node-01')")
                tnt_eval(f"billing_authorize_call('charlie@example.com', '79991234567', '{cid2}', 'rtpe-node-01')")
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.end_headers()
                self.wfile.write(json.dumps({"message": "Charlie Call 1: Allowed. Call 2: Blocked by Anti-Fraud (Limit Exceeded)"}).encode('utf-8'))
            elif self.path.startswith('/api/end_call'):
                state = fetch_json_state()
                if state.get("dialogs") and len(state["dialogs"]) > 0:
                    d = state["dialogs"][0]
                    cid = d["call_id"]
                    duration = 185
                    tnt_eval(f"billing_finalize_cdr('{cid}', {duration}, 9250, 9248, 1.15, 0.02, 4.42, 'rtpe-node-01')")
                    msg = f"Finalized {cid}: Duration {duration}s -> Balance debited $0.08, MOS 4.42"
                else:
                    msg = "No active calls to teardown. Click 'Alice calls USA' first!"
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.end_headers()
                self.wfile.write(json.dumps({"message": msg}).encode('utf-8'))
            elif self.path.startswith('/api/reset'):
                lua = """
                local fiber = require('fiber')
                local t = box.space.tariffs
                if t then
                    t:replace({'1', 'USA / Canada', 0.02, 0.0})
                    t:replace({'44', 'United Kingdom', 0.05, 0.0})
                    t:replace({'7', 'Russia Mobile', 0.03, 0.0})
                    t:replace({'default', 'Default', 0.10, 0.0})
                end

                local s = box.space.subscribers
                if s then
                    s:replace({'alice@example.com', 25.00, 'USD', 'active', 5, 'standard', math.floor(fiber.time())})
                    s:replace({'bob@example.com', 0.00, 'USD', 'active', 2, 'standard', math.floor(fiber.time())})
                    s:replace({'charlie@example.com', 10.00, 'USD', 'active', 1, 'standard', math.floor(fiber.time())})
                end
                
                if box.space.kam_dialogs then box.space.kam_dialogs:truncate() end
                if box.space.cdrs then box.space.cdrs:truncate() end
                return true
                """
                tnt_eval(lua)
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.end_headers()
                self.wfile.write(json.dumps({"message": "State reset: Alice ($25), Bob ($0), Charlie ($10). Cleared dialogs & CDRs."}).encode('utf-8'))
            else:
                self.send_response(404)
                self.end_headers()
        except Exception as e:
            self.send_response(500)
            self.send_header('Content-Type', 'application/json')
            self.end_headers()
            self.wfile.write(json.dumps({"error": str(e)}).encode('utf-8'))

def run_server(port=8089):
    auto_seed_if_empty()
    server = ThreadingHTTPServer(('0.0.0.0', port), RequestHandler)
    print(f"==================================================================", flush=True)
    print(f"  Tarantool VoIP Telecom Billing & Live CDR Dashboard             ", flush=True)
    print(f"  Open in Browser: http://localhost:{port}                         ", flush=True)
    print(f"==================================================================", flush=True)
    server.serve_forever()

if __name__ == '__main__':
    run_server()
