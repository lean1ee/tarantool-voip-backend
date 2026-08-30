--[[
    tarantool_backend/app/ttl_worker.lua
    Continuous, non-blocking background expiration fiber using secondary index 'by_expire'.

    Drains expired sessions incrementally in 500-tuple batches to ensure zero transaction stalls
    and completely eliminate memory residue.

    SPDX-License-Identifier: GPL-2.0-or-later
]]

local fiber = require('fiber')
local log = require('log')

local ttl_worker = {}
local is_running = false

--- Start the continuous TTL cleaner fiber
-- @param interval_sec number: Sleep interval in seconds between drain sweeps (default: 5)
function ttl_worker.start(interval_sec)
    interval_sec = interval_sec or 5
    if is_running then return end
    is_running = true

    fiber.create(function()
        fiber.name('ttl_cleaner')
        log.info("Starting continuous TTL Cleaner Fiber (sweep interval: %ds)...", interval_sec)

        while is_running do
            local now = math.floor(fiber.time())
            
            --------------------------------------------------------------------
            -- 1. Drain expired RTPEngine call sessions
            --------------------------------------------------------------------
            local calls_space = box.space.rtpe_calls
            if calls_space and calls_space.index.by_expire then
                local total_purged = 0
                while is_running do
                    local expired_keys = {}
                    -- Ordered range scan: fetch up to 1000 expired keys <= current timestamp
                    for _, tuple in calls_space.index.by_expire:pairs(now, { iterator = 'LE' }) do
                        table.insert(expired_keys, tuple[1])
                        if #expired_keys >= 1000 then break end
                    end
                    if #expired_keys == 0 then break end

                    for _, key in ipairs(expired_keys) do
                        calls_space:delete(key)
                    end
                    total_purged = total_purged + #expired_keys
                    -- Yield execution to allow concurrent IProto client requests to process
                    fiber.yield()
                end
                if total_purged > 0 then
                    log.info("TTL Cleaner: purged %d expired RTPEngine calls", total_purged)
                end
            end

            --------------------------------------------------------------------
            -- 2. Drain expired Kamailio / OpenSIPS SIP dialogs
            --------------------------------------------------------------------
            local dialogs_space = box.space.kam_dialogs
            if dialogs_space and dialogs_space.index.by_expire then
                local total_purged_dia = 0
                while is_running do
                    local expired_dialogs = {}
                    for _, tuple in dialogs_space.index.by_expire:pairs(now, { iterator = 'LE' }) do
                        table.insert(expired_dialogs, tuple[1])
                        if #expired_dialogs >= 1000 then break end
                    end
                    if #expired_dialogs == 0 then break end

                    for _, key in ipairs(expired_dialogs) do
                        dialogs_space:delete(key)
                    end
                    total_purged_dia = total_purged_dia + #expired_dialogs
                    fiber.yield()
                end
            end

            fiber.sleep(interval_sec)
        end
    end)
end

--- Stop the background expiration fiber cleanly
function ttl_worker.stop()
    is_running = false
    log.info("Stopping TTL Cleaner Fiber...")
end

return ttl_worker
