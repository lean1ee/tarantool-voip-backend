--[[
    tarantool_backend/app/schema.lua
    Carrier-grade Tarantool 3.x schema definition for Kamailio, OpenSIPS & RTPEngine.

    Spaces defined:
      - rtpe_calls:    Active RTPEngine media sessions and call legs
      - kam_dialogs:   Active SIP dialogs for Kamailio / OpenSIPS
      - kam_usrloc:    User location registrations
      - cluster_nodes: Heartbeat and load-balancing state for media relays

    SPDX-License-Identifier: GPL-2.0-or-later
]]

local log = require('log')

local schema = {}

--- Initialize all Memtx spaces, formats, and indexes
function schema.init()
    log.info("Initializing Tarantool schema for Kamailio + OpenSIPS + RTPEngine...")

    ----------------------------------------------------------------------------
    -- 1. Space: rtpe_calls (Space ID: 512)
    -- Stores active RTPEngine media sessions with primary key and secondary indexes
    ----------------------------------------------------------------------------
    local calls = box.schema.space.create('rtpe_calls', {
        id = 512,
        if_not_exists = true,
        format = {
            { name = 'call_id',    type = 'string' },   -- SIP Call-ID (Primary Key)
            { name = 'node_id',    type = 'string' },   -- RTPEngine instance identifier
            { name = 'state',      type = 'string' },   -- Session state: 'active', 'closing', 'terminated'
            { name = 'created_at', type = 'unsigned' }, -- Creation epoch timestamp
            { name = 'updated_at', type = 'unsigned' }, -- Last modification timestamp
            { name = 'expires_at', type = 'unsigned' }, -- Expiration deadline for TTL cleanup
            { name = 'payload',    type = 'any' },      -- SDP parameters, port maps, codecs
        }
    })

    -- Primary Index: call_id (O(log N) lookup by Call-ID)
    calls:create_index('primary', {
        parts = { 'call_id' },
        if_not_exists = true,
    })

    -- Secondary Index: node_id + updated_at
    -- Enables instant O(log N) failover retrieval of all sessions belonging to a failed media relay
    calls:create_index('by_node', {
        parts = { 'node_id', 'updated_at' },
        if_not_exists = true,
        unique = false,
    })

    -- Secondary Index: expires_at
    -- Enables ordered, non-blocking range scan for continuous TTL cleanup fiber
    calls:create_index('by_expire', {
        parts = { 'expires_at' },
        if_not_exists = true,
        unique = false,
    })

    ----------------------------------------------------------------------------
    -- 2. Space: kam_dialogs (Space ID: 514)
    -- Stores active SIP dialogs synchronized across Kamailio and OpenSIPS proxies
    ----------------------------------------------------------------------------
    local dialogs = box.schema.space.create('kam_dialogs', {
        id = 514,
        if_not_exists = true,
        format = {
            { name = 'call_id',    type = 'string' },
            { name = 'from_tag',   type = 'string' },
            { name = 'to_tag',     type = 'string' },
            { name = 'state',      type = 'unsigned' },
            { name = 'expires_at', type = 'unsigned' },
            { name = 'extra_data', type = 'any', is_nullable = true },
        }
    })

    dialogs:create_index('primary', {
        parts = { 'call_id' },
        if_not_exists = true,
    })

    dialogs:create_index('by_expire', {
        parts = { 'expires_at' },
        if_not_exists = true,
        unique = false,
    })

    ----------------------------------------------------------------------------
    -- 3. Space: kam_usrloc (Space ID: 515)
    -- Stores SIP subscriber registrations (UsrLoc location table)
    ----------------------------------------------------------------------------
    local usrloc = box.schema.space.create('kam_usrloc', {
        id = 515,
        if_not_exists = true,
        format = {
            { name = 'contact_key', type = 'string' },
            { name = 'username',    type = 'string' },
            { name = 'domain',      type = 'string' },
            { name = 'contact',     type = 'string' },
            { name = 'received',    type = 'string', is_nullable = true },
            { name = 'expires_at',  type = 'unsigned' },
            { name = 'socket',      type = 'string', is_nullable = true },
            { name = 'user_agent',  type = 'string', is_nullable = true },
        }
    })

    usrloc:create_index('primary', {
        parts = { 'contact_key' },
        if_not_exists = true,
    })

    usrloc:create_index('by_user', {
        parts = { 'username', 'domain' },
        if_not_exists = true,
        unique = false,
    })

    usrloc:create_index('by_expire', {
        parts = { 'expires_at' },
        if_not_exists = true,
        unique = false,
    })

    ----------------------------------------------------------------------------
    -- 4. Space: cluster_nodes (Space ID: 513)
    -- Tracks health, active call count, and availability of RTPEngine media nodes
    ----------------------------------------------------------------------------
    local nodes = box.schema.space.create('cluster_nodes', {
        id = 513,
        if_not_exists = true,
        format = {
            { name = 'node_id',     type = 'string' },
            { name = 'address',     type = 'string' },
            { name = 'status',      type = 'string' },
            { name = 'active_calls',type = 'unsigned' },
            { name = 'last_ping',   type = 'unsigned' },
        }
    })

    ----------------------------------------------------------------------------
    -- 5. Space: subscribers (Space ID: 516)
    -- Prepaid / postpaid subscriber profiles, real-time balances, and limits
    ----------------------------------------------------------------------------
    local subs = box.schema.space.create('subscribers', {
        id = 516,
        if_not_exists = true,
        format = {
            { name = 'subscriber_id',       type = 'string' },   -- E.164 phone or SIP username (PK)
            { name = 'balance',             type = 'number' },   -- Real-time monetary balance
            { name = 'currency',            type = 'string' },   -- Currency code ('USD', 'EUR', 'RUB')
            { name = 'status',              type = 'string' },   -- 'active', 'suspended', 'blocked'
            { name = 'max_concurrent_calls',type = 'unsigned' }, -- Anti-fraud limit
            { name = 'tariff_id',           type = 'string' },   -- Assigned tariff plan
            { name = 'updated_at',          type = 'unsigned' }, -- Last balance update epoch
        }
    })

    subs:create_index('primary', {
        parts = { 'subscriber_id' },
        if_not_exists = true,
    })

    ----------------------------------------------------------------------------
    -- 6. Space: tariffs (Space ID: 517)
    -- Rate table for number prefixes, connect fees, and per-minute rating
    ----------------------------------------------------------------------------
    local tariffs = box.schema.space.create('tariffs', {
        id = 517,
        if_not_exists = true,
        format = {
            { name = 'prefix',          type = 'string' }, -- Destination prefix ('1', '7', '44', 'default')
            { name = 'description',     type = 'string' }, -- Destination name ('USA/Canada', 'Russia Mobile')
            { name = 'cost_per_minute', type = 'number' }, -- Cost per minute in base currency
            { name = 'connect_fee',     type = 'number' }, -- One-time connection fee
        }
    })

    tariffs:create_index('primary', {
        parts = { 'prefix' },
        if_not_exists = true,
    })

    ----------------------------------------------------------------------------
    -- 7. Space: cdrs (Space ID: 518)
    -- Real-time Call Detail Records with duration, billing, and RTPEngine MOS quality
    ----------------------------------------------------------------------------
    local cdrs = box.schema.space.create('cdrs', {
        id = 518,
        if_not_exists = true,
        format = {
            { name = 'cdr_id',          type = 'string' },   -- Unique CDR identifier (PK)
            { name = 'call_id',         type = 'string' },   -- SIP Call-ID
            { name = 'caller',          type = 'string' },   -- Originating subscriber
            { name = 'callee',          type = 'string' },   -- Destination number
            { name = 'start_time',      type = 'unsigned' }, -- Call answer epoch
            { name = 'end_time',        type = 'unsigned' }, -- Call teardown epoch
            { name = 'duration_sec',    type = 'unsigned' }, -- Billable duration in seconds
            { name = 'billed_amount',   type = 'number' },   -- Total amount deducted from balance
            { name = 'mos_score',       type = 'number' },   -- Mean Opinion Score (1.0 - 5.0) from RTPEngine
            { name = 'jitter_ms',       type = 'number' },   -- RTP packet jitter in ms
            { name = 'packet_loss_pct', type = 'number' },   -- Media packet loss percentage
            { name = 'node_id',         type = 'string' },   -- Media relay node
        }
    })

    cdrs:create_index('primary', {
        parts = { 'cdr_id' },
        if_not_exists = true,
    })

    cdrs:create_index('by_call_id', {
        parts = { 'call_id' },
        if_not_exists = true,
        unique = false,
    })

    cdrs:create_index('by_caller', {
        parts = { 'caller', 'end_time' },
        if_not_exists = true,
        unique = false,
    })

    log.info("Tarantool schema initialized successfully.")
end

return schema
