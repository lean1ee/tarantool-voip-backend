--[[
    tarantool_backend/init.lua
    Main entry point and runtime bootstrap for Tarantool 3.x VoIP backend.

    Configures:
      - In-memory Memtx engine (512 MB slab arena)
      - Write-Ahead Logging (Streaming WAL mode for guaranteed zero audio jitter)
      - Non-blocking IProto network listener on port 3301
      - Global stored procedure exports and background cleanup fibers

    SPDX-License-Identifier: GPL-2.0-or-later
]]

box.cfg {
    listen = os.getenv('TARANTOOL_LISTEN') or '0.0.0.0:3301',
    memtx_memory = 512 * 1024 * 1024, -- 512 MB in-memory slab arena
    wal_mode = 'write',               -- Continuous streaming WAL (no fork/COW latency spikes)
    log_level = 5,
}

local schema = require('app.schema')
local rtpe_service = require('app.rtpe_service')
local billing_service = require('app.billing_service')
local asterisk_service = require('app.asterisk_service')
local ttl_worker = require('app.ttl_worker')

-- 1. Initialize space formats and secondary indexes
schema.init()
asterisk_service.init()

-- 2. Export global RPC stored procedures for direct IProto execution (box.call)
rawset(_G, 'rtpe_call_upsert', rtpe_service.call_upsert)
rawset(_G, 'rtpe_call_delete', rtpe_service.call_delete)
rawset(_G, 'rtpe_call_get', rtpe_service.call_get)
rawset(_G, 'rtpe_call_restore', rtpe_service.call_restore)
rawset(_G, 'rtpe_select_node', rtpe_service.select_optimal_node)

-- Real-time Billing, Anti-Fraud, and CDR Procedures
rawset(_G, 'billing_authorize_call', billing_service.authorize_call)
rawset(_G, 'billing_finalize_cdr', billing_service.finalize_cdr)
rawset(_G, 'billing_get_live_stats', billing_service.get_live_stats)

-- 3. Configure authentication users and permissions
if not box.schema.user.exists('rtpe_user') then
    box.schema.user.create('rtpe_user', { password = 'rtpe_secret_password' })
    box.schema.user.grant('rtpe_user', 'read,write,execute', 'universe')
end
box.schema.user.grant('guest', 'read,write,execute', 'universe', nil, { if_not_exists = true })

-- 4. Start the continuous non-blocking background TTL expiration fiber
ttl_worker.start(5)

print("Tarantool Kamailio/RTPEngine Backend started successfully on " .. box.cfg.listen)
