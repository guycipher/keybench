-- The valsize workload is the mixed read write blend run across a sweep of value
-- sizes. Where mixed fixes the value at a constant, valsize declares a sweep over
-- ctx.valuebytes and stores values of that size, so its curve shows how an engine
-- responds as records grow from small values into large blobs, the regime where
-- separating values from the LSM, RocksDB BlobDB or the TidesDB key log, begins to
-- matter. It is the second user of the general per workload sweep, proof that the
-- swept parameter need not be batch.
--
-- The dataset is items times the current value size, so the largest size times
-- items is the working set. Keep items modest when the sweep reaches into the
-- megabytes, or the big end will not fit the drive.

local assert, type = assert, type
local random = math.random
local format, rep = string.format, string.rep
local kv_get, kv_put, kv_del, kv_range, kv_mput = kv.get, kv.put, kv.del, kv.range, kv.mput

local KEY_FMT = "k:%012d"

-- An operation is chosen per call by a percentage roll against these cumulative
-- thresholds, the same read mostly mix as mixed. SCAN_WINDOW is how many keys one
-- range scan covers.
local PCT         = 100
local W_GET       = 50
local W_PUT       = 80
local W_DEL       = 90
local SCAN_WINDOW = 100

-- Seeding groups this many writes into one kv_mput. It is kept small because the
-- values here can be large, so a batch commits a bounded number of bytes at once.
local SEED_BATCH = 64

-- The value string is rebuilt only when the swept size changes, which within one
-- measurement is never after the first call, so the hot path allocates nothing.
-- Each worker has its own Lua state, so this cache is private to the worker.
local cached_value, cached_size

local function value_for(sz)
  assert(type(sz) == "number" and sz >= 1, "ctx.valuebytes must be a number >= 1")
  if cached_size ~= sz then
    cached_value = rep("v", sz)
    cached_size = sz
  end
  return cached_value
end

local function key(i)
  assert(type(i) == "number" and i >= 0, "key index must be a non-negative number")
  return format(KEY_FMT, i)
end

local function rnd(n)
  assert(type(n) == "number" and n >= 1, "rnd bound must be >= 1")
  return random(0, n - 1)
end

-- Seeding is sharded across the worker threads and batched, as in mixed, but the
-- value size comes from the swept ctx.valuebytes so each point seeds a dataset of
-- that record size.
local function load(ctx)
  assert(type(ctx) == "table", "ctx must be a table")
  assert(type(ctx.items) == "number" and ctx.items >= 1, "ctx.items must be >= 1")
  assert(type(ctx.threads) == "number" and ctx.threads >= 1, "ctx.threads must be >= 1")
  local value = value_for(ctx.valuebytes)
  local b = SEED_BATCH
  local buf = {}
  for j = 1, b do buf[j] = { key = "", val = value } end
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
    kv_get(key(rnd(n)))
  elseif roll <= W_PUT then
    kv_put(key(rnd(n)), value_for(ctx.valuebytes))
  elseif roll <= W_DEL then
    kv_del(key(rnd(n)))
  else
    local start = rnd(n)
    kv_range(key(start), key(start + SCAN_WINDOW), SCAN_WINDOW)
  end
end

-- valsize sweeps the value size from a small record to a one megabyte blob. The
-- harness iterates these, injects each as ctx.valuebytes, and labels the report
-- column valuebytes.
return {
  name = "valsize",
  load = load,
  run = run,
  sweep = { param = "valuebytes", values = { 256, 4096, 65536, 262144, 1048576 } },
}
