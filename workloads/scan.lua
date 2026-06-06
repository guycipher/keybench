-- The scan workload exercises kv.scan, the streaming range read. Where kv.range
-- materialises the whole result as a Lua array, kv.scan invokes a callback per
-- row and never builds a table, so a scan of millions of rows uses constant Lua
-- memory. The callback returns a truthy value to stop early, and it must not call
-- kv.put or kv.del. An engine that scans under its own read lock, such as the
-- skiplist, would deadlock on the reentrant write.

local assert, type = assert, type
local random, max = math.random, math.max
local format, tonumber = string.format, tonumber
local kv_scan, kv_mput = kv.scan, kv.mput

local KEY_FMT     = "k:%012d"
local SCAN_WINDOW = 1000

-- Stored values are a fixed size opaque payload, built once per worker so the
-- hot path allocates no per call value. The dataset is items times this size,
-- so raise --items to scale the scanned set into the gigabytes. A repeated byte
-- compresses away, so size a dataset this way only with compression off.
local VALUE_BYTES = 4096
local VALUE       = string.rep("v", VALUE_BYTES)

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

-- Seeding is sharded across the worker threads and batched. Each thread writes
-- the slice of the keyspace it owns, ctx.thread of every ctx.threads, grouping
-- ctx.batch keys into one kv_mput so a large seed is parallel and amortized.
local function load(ctx)
  assert(type(ctx) == "table", "ctx must be a table")
  assert(type(ctx.items) == "number" and ctx.items >= 1, "ctx.items must be >= 1")
  assert(type(ctx.threads) == "number" and ctx.threads >= 1, "ctx.threads must be >= 1")
  local b = max(1, ctx.batch)
  local buf = {}
  for j = 1, b do buf[j] = { key = "", val = VALUE } end
  local n = 0
  for i = ctx.thread, ctx.items - 1, ctx.threads do
    n = n + 1
    buf[n].key = key(i)
    if n == b then kv_mput(buf); n = 0 end
  end
  if n > 0 then
    local last = {}
    for j = 1, n do last[j] = buf[j] end
    kv_mput(last)
  end
end

local function run(ctx)
  assert(type(ctx) == "table", "ctx must be a table")
  local n = ctx.items
  assert(type(n) == "number" and n >= 1, "ctx.items must be >= 1")
  local start = random(0, max(0, n - SCAN_WINDOW))
  acc.sum = 0
  kv_scan(key(start), key(start + SCAN_WINDOW), scan_add)
end

return { name = "scan", load = load, run = run }
