-- The mixed workload is a deliberately plain baseline. It issues a uniform random
-- mix of reads, writes, deletes, and short range scans with no business logic, so
-- its numbers reflect raw engine behaviour rather than any workload structure.
--
-- run() takes the keyspace size from its ctx argument on every call rather than
-- from a value captured in load(). Each worker thread runs in its own Lua state
-- and only the loader state ever executes load(), so an upvalue assigned there
-- would be nil inside a worker.
--
-- Some of the assertions below run inside the measured run() path, so their cost
-- is part of the reported throughput. Because that cost is identical for every
-- engine it cancels out when engines are compared and only shifts the absolutes.

local assert, type = assert, type
local random, max = math.random, math.max
local format = string.format
local kv_get, kv_put, kv_del, kv_range, kv_mput = kv.get, kv.put, kv.del, kv.range, kv.mput

-- The store orders keys by raw byte comparison, so an index is formatted to a
-- fixed zero padded width to make that byte order agree with numeric order.
-- Without the padding the key for two would sort after the key for ten.
local KEY_FMT = "k:%012d"

-- Stored values are a fixed size opaque payload, built once per worker so the
-- hot path allocates no per call value. The dataset is items times this size,
-- so raise --items to scale the working set into the gigabytes. A repeated byte
-- compresses away, so size a dataset this way only with compression off.
local VALUE_BYTES = 4096
local VALUE       = string.rep("v", VALUE_BYTES)

-- An operation is chosen per call by a percentage roll against these cumulative
-- thresholds, weighted to resemble a read mostly service. SCAN_WINDOW is the
-- number of keys one range scan covers.
local PCT         = 100
local W_GET       = 50
local W_PUT       = 80
local W_DEL       = 90
local SCAN_WINDOW = 100

local function key(i)
  assert(type(i) == "number" and i >= 0, "key index must be a non-negative number")
  return format(KEY_FMT, i)
end

local function rnd(n)
  assert(type(n) == "number" and n >= 1, "rnd bound must be >= 1")
  local v = random(0, n - 1)
  assert(v >= 0 and v < n, "rnd result fell outside its range")
  return v
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
  local roll = random(PCT)
  if roll <= W_GET then
    -- The returned value is intentionally discarded. This call exists only to time a read.
    kv_get(key(rnd(n)))
  elseif roll <= W_PUT then
    kv_put(key(rnd(n)), VALUE)
  elseif roll <= W_DEL then
    kv_del(key(rnd(n)))
  else
    local start = rnd(n)
    kv_range(key(start), key(start + SCAN_WINDOW), SCAN_WINDOW)
  end
end

return { name = "mixed", load = load, run = run }
