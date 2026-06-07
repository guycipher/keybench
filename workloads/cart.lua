-- The cart workload models an Amazon style shopping cart on a sorted key value
-- store, in the spirit of the Dynamo paper where a cart must always accept
-- writes. Rather than store a whole cart as one value, it stores one key per
-- line item under a per user prefix, which turns "view cart" into a single range
-- scan and makes the sorted store design pay off.
--
-- As in mixed.lua, run() derives everything from its ctx argument so it behaves
-- correctly inside a worker that never executes load(). The helpers below are
-- pure functions of their arguments for the same reason, and their assertions
-- run in the measured path, adding a cost that is constant across engines.

local assert, type = assert, type
local random, floor, max = math.random, math.floor, math.max
local format = string.format
local ipairs = ipairs
local kv_get, kv_put, kv_del, kv_range, kv_mput = kv.get, kv.put, kv.del, kv.range, kv.mput

-- A real shopper touches a small, stable set of products, not the whole catalog.
-- pick_item maps a user id through a multiplicative hash to a fixed window of
-- CART_W catalog items, so the same user keeps hitting the same handful and reads
-- and deletes land on items that user actually added.
local CART_W     = 16
local KNUTH_MULT = 2654435761

-- Real traffic is skewed toward a few hot users. Raising a uniform random number
-- to a power above one cheaply biases selection toward low ids without building a
-- Zipf table. A HOT_SKEW of one would make selection uniform again.
local HOT_SKEW = 2.0

-- Item ids are zero padded to a fixed width so the store's byte ordering of keys
-- matches their numeric ordering.
local LINE_KEY_FMT = "u:%010d:c:%010d"

-- A line item value is a fixed size opaque payload, built once per worker so the
-- hot path allocates no per call value. Cart size on disk grows with users and
-- with this size, so raise --users to scale into the gigabytes. A repeated byte
-- compresses away, so size a dataset this way only with compression off.
local VALUE_BYTES  = 4096
local VALUE        = string.rep("v", VALUE_BYTES)

-- A user's line items all share a per user prefix that ends in a colon byte.
-- The next byte value after that colon is the semicolon, so a lower bound ending
-- in the colon and an upper bound ending in the semicolon form a half open range
-- that covers exactly that one user's items.
local BOUNDS_LO_FMT = "u:%010d:c:"
local BOUNDS_HI_FMT = "u:%010d:c;"

-- Each user is seeded with SEED_ITEMS_MIN starting items plus up to SEED_ITEMS_RAND
-- more, so carts are not empty when the measured phase begins.
local SEED_ITEMS_MIN  = 3
local SEED_ITEMS_RAND = 5

-- Cumulative percentage thresholds for the operation mix, weighted to resemble
-- real cart traffic. Most calls are adds and views, fewer are single reads and
-- edits, with the occasional remove and a rare checkout.
local PCT      = 100
local W_ADD    = 35
local W_VIEW   = 60
local W_GET    = 75
local W_UPDATE = 85
local W_REMOVE = 95

local function pick_item(uid, items)
  assert(type(uid) == "number" and uid >= 0, "uid must be a non-negative number")
  assert(type(items) == "number" and items >= 1, "items must be >= 1")
  local span = max(1, items - CART_W)
  local base = (uid * KNUTH_MULT) % span
  return base + random(0, CART_W - 1)
end

local function hot_user(users)
  assert(type(users) == "number" and users >= 1, "users must be >= 1")
  local uid = floor(users * (random() ^ HOT_SKEW))
  assert(uid >= 0 and uid < users, "hot_user produced an out-of-range id")
  return uid
end

local function line_key(uid, iid)
  assert(type(uid) == "number" and type(iid) == "number", "ids must be numbers")
  return format(LINE_KEY_FMT, uid, iid)
end

local function cart_bounds(uid)
  assert(type(uid) == "number" and uid >= 0, "uid must be a non-negative number")
  return format(BOUNDS_LO_FMT, uid), format(BOUNDS_HI_FMT, uid)
end

-- Seeding groups this many line items into one kv_mput. It belongs to the load
-- phase, not the measured run, so it is fixed here like VALUE_BYTES rather than
-- driven by a flag.
local SEED_BATCH = 512

-- Seeding is sharded across the worker threads and batched. Each thread seeds
-- the users it owns, ctx.thread of every ctx.threads, grouping SEED_BATCH line
-- items into one kv_mput so a large seed is parallel and amortized.
local function load(ctx)
  assert(type(ctx) == "table", "ctx must be a table")
  local users, items = ctx.users, ctx.items
  assert(type(users) == "number" and type(items) == "number", "users/items must be numbers")
  assert(type(ctx.threads) == "number" and ctx.threads >= 1, "ctx.threads must be >= 1")
  local b = SEED_BATCH
  local buf = {}
  for j = 1, b do buf[j] = { key = "", val = VALUE } end
  local n = 0
  for uid = ctx.thread, users - 1, ctx.threads do
    local count = SEED_ITEMS_MIN + random(0, SEED_ITEMS_RAND)
    for _ = 1, count do
      n = n + 1
      buf[n].key = line_key(uid, pick_item(uid, items))
      if n == b then kv_mput(buf); n = 0 end
    end
  end
  if n > 0 then
    local last = {}
    for j = 1, n do last[j] = buf[j] end
    kv_mput(last)
  end
end

local function run(ctx)
  assert(type(ctx) == "table", "ctx must be a table")
  local users, items = ctx.users, ctx.items
  assert(type(users) == "number" and type(items) == "number", "users/items must be numbers")
  local uid = hot_user(users)
  local roll = random(PCT)
  if roll <= W_ADD then
    kv_put(line_key(uid, pick_item(uid, items)), VALUE)
  elseif roll <= W_VIEW then
    local lo, hi = cart_bounds(uid)
    kv_range(lo, hi)
  elseif roll <= W_GET then
    kv_get(line_key(uid, pick_item(uid, items)))
  elseif roll <= W_UPDATE then
    kv_put(line_key(uid, pick_item(uid, items)), VALUE)
  elseif roll <= W_REMOVE then
    kv_del(line_key(uid, pick_item(uid, items)))
  else
    -- Checkout scans the user's whole cart, then deletes each line. kv.range
    -- returns a fresh array built by the harness, which this loop cannot avoid.
    local lo, hi = cart_bounds(uid)
    local lines = kv_range(lo, hi)
    for _, line in ipairs(lines) do
      kv_del(line.key)
    end
  end
end

return { name = "cart", load = load, run = run }
