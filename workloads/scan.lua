-- The scan workload exercises kv.scan, the streaming range read. Where kv.range
-- materialises the whole result as a Lua array, kv.scan invokes a callback per
-- row and never builds a table, so a scan of millions of rows uses constant Lua
-- memory. The callback returns a truthy value to stop early, and it must not call
-- kv.put or kv.del. An engine that scans under its own read lock, such as the
-- skiplist, would deadlock on the reentrant write.

local assert, type = assert, type
local random, min, max = math.random, math.min, math.max
local format, tonumber, tostring = string.format, tonumber, tostring
local kv_put, kv_scan = kv.put, kv.scan

local KEY_FMT       = "k:%012d"
local SEED_KEYS_MAX = 200000
local SCAN_WINDOW   = 1000

-- The accumulator and the callback are defined once at module scope and reused on
-- every run() so the hot path allocates neither a fresh closure nor a fresh
-- table. Each worker has its own Lua state, so this state is not shared.
local acc = { sum = 0 }

local function scan_add(_, val)
  assert(val ~= nil, "scan value must not be nil")
  acc.sum = acc.sum + (tonumber(val) or 0)
  return false
end

local function key(i)
  assert(type(i) == "number" and i >= 0, "key index must be a non-negative number")
  return format(KEY_FMT, i)
end

local function load(ctx)
  assert(type(ctx) == "table", "ctx must be a table")
  assert(type(ctx.items) == "number", "ctx.items must be a number")
  local n = min(ctx.items, SEED_KEYS_MAX)
  for i = 0, n - 1 do
    kv_put(key(i), tostring(i))
  end
end

local function run(ctx)
  assert(type(ctx) == "table", "ctx must be a table")
  local n = min(ctx.items, SEED_KEYS_MAX)
  assert(n >= 0, "keyspace must be non-negative")
  local start = random(0, max(0, n - SCAN_WINDOW))
  acc.sum = 0
  kv_scan(key(start), key(start + SCAN_WINDOW), scan_add)
end

return { name = "scan", load = load, run = run }
