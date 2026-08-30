--[[
  tarantool_backend/app/asterisk_service.lua
  Asterisk Realtime (Sorcery/ARA) and High-Speed CDR Stored Procedures for Tarantool 3.x
--]]

local json = require('json')
local log = require('log')

local M = {}

function M.init()
    -- 1. Space: ps_endpoints (Space ID: 516)
    if not box.space.ps_endpoints then
        local s = box.schema.space.create('ps_endpoints', {
            id = 516,
            if_not_exists = true,
            engine = 'memtx',
            format = {
                { name = 'id', type = 'string' },
                { name = 'transport', type = 'string', is_nullable = true },
                { name = 'aors', type = 'string', is_nullable = true },
                { name = 'auth', type = 'string', is_nullable = true },
                { name = 'context', type = 'string', is_nullable = true },
                { name = 'disallow', type = 'string', is_nullable = true },
                { name = 'allow', type = 'string', is_nullable = true },
                { name = 'direct_media', type = 'string', is_nullable = true },
                { name = 'data_json', type = 'string', is_nullable = true },
            }
        })
        s:create_index('primary', { parts = { 'id' }, if_not_exists = true })
    end

    -- 2. Space: ps_auths (Space ID: 517)
    if not box.space.ps_auths then
        local s = box.schema.space.create('ps_auths', {
            id = 517,
            if_not_exists = true,
            engine = 'memtx',
            format = {
                { name = 'id', type = 'string' },
                { name = 'auth_type', type = 'string', is_nullable = true },
                { name = 'password', type = 'string', is_nullable = true },
                { name = 'username', type = 'string', is_nullable = true },
                { name = 'realm', type = 'string', is_nullable = true },
            }
        })
        s:create_index('primary', { parts = { 'id' }, if_not_exists = true })
    end

    -- 3. Space: ps_aors (Space ID: 518)
    if not box.space.ps_aors then
        local s = box.schema.space.create('ps_aors', {
            id = 518,
            if_not_exists = true,
            engine = 'memtx',
            format = {
                { name = 'id', type = 'string' },
                { name = 'max_contacts', type = 'unsigned', is_nullable = true },
                { name = 'remove_existing', type = 'string', is_nullable = true },
                { name = 'default_expiration', type = 'unsigned', is_nullable = true },
            }
        })
        s:create_index('primary', { parts = { 'id' }, if_not_exists = true })
    end

    -- 4. Space: asterisk_cdrs (Space ID: 519)
    if not box.space.asterisk_cdrs then
        local s = box.schema.space.create('asterisk_cdrs', {
            id = 519,
            if_not_exists = true,
            engine = 'memtx',
            format = {
                { name = 'uniqueid', type = 'string' },
                { name = 'accountcode', type = 'string', is_nullable = true },
                { name = 'src', type = 'string', is_nullable = true },
                { name = 'dst', type = 'string', is_nullable = true },
                { name = 'dcontext', type = 'string', is_nullable = true },
                { name = 'clid', type = 'string', is_nullable = true },
                { name = 'channel', type = 'string', is_nullable = true },
                { name = 'dstchannel', type = 'string', is_nullable = true },
                { name = 'lastapp', type = 'string', is_nullable = true },
                { name = 'lastdata', type = 'string', is_nullable = true },
                { name = 'start_time', type = 'string', is_nullable = true },
                { name = 'answer_time', type = 'string', is_nullable = true },
                { name = 'end_time', type = 'string', is_nullable = true },
                { name = 'duration', type = 'unsigned', is_nullable = true },
                { name = 'billsec', type = 'unsigned', is_nullable = true },
                { name = 'disposition', type = 'integer', is_nullable = true },
                { name = 'userfield', type = 'string', is_nullable = true },
                { name = 'created_at', type = 'number' },
            }
        })
        s:create_index('primary', { parts = { 'uniqueid' }, if_not_exists = true })
        s:create_index('by_src', { parts = { 'src' }, unique = false, if_not_exists = true })
    end

    log.info('[asterisk_service] Asterisk Realtime & CDR spaces initialized (516-519)')
end

-- Global stored procedures for Asterisk Realtime engine

function _G.ast_realtime_get(table_name, key_field, key_val)
    local sp = box.space[table_name]
    if not sp then
        return {}
    end

    local tuple = sp:get({ key_val })
    if not tuple then
        return {}
    end

    local map = {}
    local format = sp:format()
    for idx, field in ipairs(format) do
        if tuple[idx] ~= nil then
            map[field.name] = tostring(tuple[idx])
        end
    end
    return { map }
end

function _G.ast_realtime_update(table_name, key_field, key_val, fields_table)
    local sp = box.space[table_name]
    if not sp then
        return -1
    end

    local tuple = sp:get({ key_val })
    if not tuple then
        return -1
    end

    local ops = {}
    local format = sp:format()
    for idx, field in ipairs(format) do
        if fields_table[field.name] ~= nil then
            table.insert(ops, { '=', idx, fields_table[field.name] })
        end
    end

    if #ops > 0 then
        sp:update({ key_val }, ops)
    end
    return 0
end

function _G.ast_realtime_update2(table_name, lookup_fields, update_fields)
    local sp = box.space[table_name]
    if not sp or not lookup_fields['id'] then
        return -1
    end

    return _G.ast_realtime_update(table_name, 'id', lookup_fields['id'], update_fields)
end

function _G.ast_realtime_store(table_name, fields_table)
    local sp = box.space[table_name]
    if not sp or not fields_table['id'] then
        return -1
    end

    local format = sp:format()
    local tuple = {}
    for idx, field in ipairs(format) do
        tuple[idx] = fields_table[field.name]
    end

    sp:replace(tuple)
    return 0
end

function _G.ast_realtime_destroy(table_name, key_field, key_val)
    local sp = box.space[table_name]
    if not sp then
        return -1
    end

    sp:delete({ key_val })
    return 0
end

function _G.ast_cdr_save(uniqueid, accountcode, src, dst, dcontext, clid, channel, dstchannel,
                         lastapp, lastdata, start_time, answer_time, duration, billsec, disposition,
                         userfield, space_name)
    local sp = box.space[space_name or 'asterisk_cdrs'] or box.space.asterisk_cdrs
    if not sp then
        return -1
    end

    local now = os.time()
    local uid = (uniqueid and uniqueid ~= '') and uniqueid or ('ast-' .. tostring(now) .. '-' .. tostring(math.random(1000, 9999)))

    sp:replace({
        uid,
        accountcode or '',
        src or '',
        dst or '',
        dcontext or '',
        clid or '',
        channel or '',
        dstchannel or '',
        lastapp or '',
        lastdata or '',
        start_time or '',
        answer_time or '',
        tostring(now),
        duration or 0,
        billsec or 0,
        disposition or 0,
        userfield or '',
        now
    })
    return 0
end

return M
