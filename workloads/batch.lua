-- The batch workload exercises the multi key verbs kv.mget and kv.mput, which
-- model how real stores amortise per operation overhead by grouping keys into one
-- call, as with a RocksDB write batch, Redis pipelining, or a multi get. Each
-- call records a single latency sample but performs ctx.batch underlying key
-- operations, so sweeping its own batch sizes traces the amortisation curve. A
-- batch size of one degrades to single key calls and gives the unbatched baseline.

local assert, type = assert, type
local random, max = math.random, math.max
local format = string.format
local kv_mget, kv_mput = kv.mget, kv.mput

local KEY_FMT = "k:%012d"
local PCT     = 100
local W_MGET  = 60

-- Stored values are a fixed size opaque payload, built once per worker so the
-- hot path allocates no per call value. The dataset is items times this size,
-- so raise --items to scale the working set into the gigabytes. A repeated byte
-- compresses away, so size a dataset this way only with compression off.
local VALUE_BYTES = 4096
local VALUE       = string.rep("v", VALUE_BYTES)

-- These buffers are reused on every run() so the hot path allocates no per call
-- tables. ctx.batch is fixed for a measurement, so each buffer grows to that size
-- once and then stays there. Each worker has its own Lua state and its own pair.
local mget_buf = {}
local mput_buf = {}

local function key(i)
  assert(type(i) == "number" and i >= 0, "key index must be a non-negative number")
  return format(KEY_FMT, i)
end

local function rnd(n)
  assert(type(n) == "number" and n >= 1, "rnd bound must be >= 1")
  return random(0, n - 1)
end

-- Seeding groups this many writes into one kv_mput. This is the load phase, so
-- it is fixed here and is independent of the batch sizes the run phase sweeps.
local SEED_BATCH = 512

-- Seeding is sharded across the worker threads and batched. Each thread writes
-- the slice of the keyspace it owns, ctx.thread of every ctx.threads, grouping
-- SEED_BATCH keys into one kv_mput so a large seed is parallel and amortized.
local function load(ctx)
  assert(type(ctx) == "table", "ctx must be a table")
  assert(type(ctx.items) == "number" and ctx.items >= 1, "ctx.items must be >= 1")
  assert(type(ctx.threads) == "number" and ctx.threads >= 1, "ctx.threads must be >= 1")
  local b = SEED_BATCH
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
  assert(type(ctx.batch) == "number" and ctx.batch >= 1, "ctx.batch must be a number >= 1")
  assert(type(ctx.items) == "number" and ctx.items >= 1, "ctx.items must be >= 1")
  local n = ctx.items
  local b = max(1, ctx.batch)
  if random(PCT) <= W_MGET then
    for j = 1, b do mget_buf[j] = key(rnd(n)) end
    kv_mget(mget_buf)
  else
    for j = 1, b do
      local pair = mput_buf[j]
      if pair == nil then pair = {}; mput_buf[j] = pair end
      pair.key = key(rnd(n))
      pair.val = VALUE
    end
    kv_mput(mput_buf)
  end
end

-- batch sweeps its own parameter, the number of keys per mget/mput call. The
-- harness iterates these values, injects each as ctx.batch, and labels the
-- report column batch. No other workload reads it. seed_per_value is false because
-- the batch size changes only the run, not the seeded data, so under seed once the
-- store is seeded once and shared across the sizes instead of reseeded per size.
return {
  name = "batch",
  load = load,
  run = run,
  sweep = { param = "batch", values = { 1, 64, 256, 512, 1024 }, seed_per_value = false },
}
