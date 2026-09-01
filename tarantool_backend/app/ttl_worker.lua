--[[
    tarantool_backend/app/ttl_worker.lua
    Continuous, non-blocking background expiration fiber using secondary index 'by_expire'.

    Drains expired sessions incrementally in 500-tuple batches to ensure zero transaction stalls
    and completely eliminate memory residue across:
      1. rtpe_calls (RTPEngine media sessions)
      2. kam_dialogs (SIP dialogs)
      3. kam_usrloc (SIP registrations)

    SPDX-License-Identifier: GPL-2.0-or-later
]]

local fiber = require('fiber')
local log = require('log')

local ttl_worker = {}
local is_running = false

local stats = {
    purged_calls = 0,
    purged_dialogs = 0,
    purged_usrloc = 0,
    last_sweep = 0,
    sweeps_count = 0,
}

--- Safely drain expired tuples from a specific space
local function drain_space(space_name, now)
    local space = box.space[space_name]
    if not space or not space.index or not space.index.by_expire then
        return 0
    end

    local purged_count = 0
    while is_running do
        local expired_keys = {}
        for _, tuple in space.index.by_expire:pairs(now, { iterator = 'LE' }) do
            table.insert(expired_keys, tuple[1])
            if #expired_keys >= 500 then break end
        end

        if #expired_keys == 0 then break end

        for _, key in ipairs(expired_keys) do
            space:delete(key)
        end
        purged_count = purged_count + #expired_keys
        fiber.yield()
    end

    return purged_count
end

--- Start the continuous TTL cleaner fiber
-- @param interval_sec number: Sleep interval in seconds between drain sweeps (default: 5)
function ttl_worker.start(interval_sec)
    interval_sec = (type(interval_sec) == 'number' and interval_sec > 0) and interval_sec or 5
    if is_running then return end
    is_running = true

    fiber.create(function()
        fiber.name('ttl_cleaner')
        log.info("Starting continuous TTL Cleaner Fiber (sweep interval: %ds)...", interval_sec)

        while is_running do
            local now = math.floor(fiber.time())
            stats.last_sweep = now
            stats.sweeps_count = stats.sweeps_count + 1

            -- 1. Drain expired RTPEngine call sessions
            local ok_calls, n_calls = pcall(drain_space, 'rtpe_calls', now)
            if ok_calls and n_calls > 0 then
                stats.purged_calls = stats.purged_calls + n_calls
                log.info("TTL Cleaner: purged %d expired RTPEngine calls", n_calls)
            end

            -- 2. Drain expired Kamailio / OpenSIPS SIP dialogs
            local ok_dia, n_dia = pcall(drain_space, 'kam_dialogs', now)
            if ok_dia and n_dia > 0 then
                stats.purged_dialogs = stats.purged_dialogs + n_dia
                log.info("TTL Cleaner: purged %d expired SIP dialogs", n_dia)
            end

            -- 3. Drain expired SIP Registrations (UsrLoc)
            local ok_loc, n_loc = pcall(drain_space, 'kam_usrloc', now)
            if ok_loc and n_loc > 0 then
                stats.purged_usrloc = stats.purged_usrloc + n_loc
                log.info("TTL Cleaner: purged %d expired UsrLoc records", n_loc)
            end

            fiber.sleep(interval_sec)
        end
    end)
end

--- Retrieve live expiration statistics
-- @return table: Statistics dictionary
function ttl_worker.get_stats()
    return {
        purged_calls   = stats.purged_calls,
        purged_dialogs = stats.purged_dialogs,
        purged_usrloc  = stats.purged_usrloc,
        last_sweep     = stats.last_sweep,
        sweeps_count   = stats.sweeps_count,
        is_running     = is_running,
    }
end

--- Stop the background expiration fiber cleanly
function ttl_worker.stop()
    is_running = false
    log.info("Stopping TTL Cleaner Fiber...")
end

return ttl_worker
