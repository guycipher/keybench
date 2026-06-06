-- The batch workload exercises the multi key verbs kv.mget and kv.mput, which
-- model how real stores amortise per operation overhead by grouping keys into one
-- call, as with a RocksDB write batch, Redis pipelining, or a multi get. Each
-- call records a single latency sample but performs ctx.batch underlying key
-- operations, so sweeping --batch traces the amortisation curve. A batch size of
-- one degrades to single key calls and gives the unbatched baseline.

local assert, type = assert, type
local random, min, max = math.random, math.min, math.max
local format, tostring = string.format, tostring
local kv_put, kv_mget, kv_mput = kv.put, kv.mget, kv.mput

local KEY_FMT       = "k:%012d"
local UPDATE_VALUE  = "updated"
local SEED_KEYS_MAX = 200000
local PCT           = 100
local W_MGET        = 60

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
  assert(type(ctx.batch) == "number" and ctx.batch >= 1, "ctx.batch must be a number >= 1")
  local n = min(ctx.items, SEED_KEYS_MAX)
  local b = max(1, ctx.batch)
  if random(PCT) <= W_MGET then
    for j = 1, b do mget_buf[j] = key(rnd(n)) end
    kv_mget(mget_buf)
  else
    for j = 1, b do
      local pair = mput_buf[j]
      if pair == nil then pair = {}; mput_buf[j] = pair end
      pair.key = key(rnd(n))
      pair.val = UPDATE_VALUE
    end
    kv_mput(mput_buf)
  end
end

return { name = "batch", load = load, run = run, batched = true }
