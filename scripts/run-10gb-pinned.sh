#!/bin/bash
# Drive the 10 GiB rocksdb vs tidesdb comparison across every bundled workload,
# then plot the whole matrix.
#
# This is scripts/run-10gb.sh adapted to a host whose kernel isolates CPUs, with
# plotting added at the end. The structure, the per workload sizing, the free
# space guard, and the one invocation per engine and workload are all from that
# script; what differs is documented below.
#
#   1. Every point runs under taskset against the P-cores and passes --pin.
#      The kernel here booted with isolcpus=0-15, so CPUs 0-15 are out of
#      scheduler load balancing. Handing several of them to a process through
#      taskset gives it a mask nothing balances across, and every thread stays on
#      whichever CPU the parent happened to be on. The run then uses exactly one
#      core no matter how many threads it was asked for, with no error and no
#      warning: throughput goes flat across the thread sweep and latency rises in
#      exact proportion to thread count, which looks like an engine that does not
#      scale. --pin places worker t on the t-th CPU of the inherited set, which is
#      what isolated CPUs are for. Measured here, skiplist scan at 8 threads:
#      565 wu/s unpinned against 4500 wu/s pinned.
#
#   2. Paths default to this machine: the repo at /root/keybench and the data on
#      /data, a Micron 7450 on xfs that is not the OS disk.
#
#   3. It plots when it finishes. Every result directory goes into one plot.py
#      invocation, so the comparison figures carry both engines, and a second
#      pass writes a single thread only set. That second set matters because
#      plot.py's throughput_compare picks the peak thread count in the data, and
#      a bar chart at the top of the sweep answers a different question than one
#      at a single thread.
#
# Before trusting a long run, rehearse the whole matrix in a couple of minutes:
#
#   KB_EXTRA="--items 50000 --users 20000 --secs 2 --threads 2 --repeat 1" \
#     scripts/run-10gb-pinned.sh
#
# Turbo must be on for the numbers to reflect the hardware. This box boots with
# it disabled, which holds the part at its 2.0 GHz base against a 5.3 GHz peak.
# The script checks and refuses to start silently handicapped.

set -uo pipefail

KB_DIR=${KB_DIR:-/root/keybench}
CONF=${CONF:-$KB_DIR/bench-configs/10gb.cnf}
RESULTS=${RESULTS:-$KB_DIR/results}
DATA=${DATA:-/data/bench}

# The 8 P-cores. Their hyperthread siblings are offline (nosmt) and CPUs 16-31
# are the E-cores, which are left out so every worker gets the same kind of core
# and the scaling curve stays readable.
PCORES=${PCORES:-0,2,4,6,8,10,12,14}

# A 10 GiB store plus the transient a compaction doubles it to needs headroom.
MIN_FREE_GIB=${MIN_FREE_GIB:-40}

KB_EXTRA=${KB_EXTRA:-}

cd "$KB_DIR" || exit 1
mkdir -p "$RESULTS" "$DATA" || exit 1

MASTER_LOG="$RESULTS/run-10gb-pinned.log"

# Refuse to spend hours measuring a clock that is not the one the hardware can
# run. no_turbo is runtime writable, so this is a one line fix rather than a
# reboot, and it is worth about 2.6x: measured 1897 MHz off against 5053 MHz on.
if [ "$(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo 0)" = "1" ]; then
  echo "refusing to start: turbo is disabled (no_turbo=1), so the cpu is pinned"
  echo "to its 2.0 GHz base instead of 5.3 GHz. enable it with:"
  echo "  echo 0 > /sys/devices/system/cpu/intel_pstate/no_turbo"
  echo "or set ALLOW_NO_TURBO=1 to measure at base clock deliberately."
  [ "${ALLOW_NO_TURBO:-0}" = "1" ] || exit 1
fi

echo "=== run-10gb-pinned started $(date -Is) ===" | tee -a "$MASTER_LOG"
echo "  cpus=$PCORES  pinned=yes  data=$DATA  conf=$CONF" | tee -a "$MASTER_LOG"

# The sizing knobs each workload needs to reach 10 GiB. Values are 4096 bytes
# everywhere except valsize, which sweeps the size itself, so 10 GiB is 2621440
# records. cart reaches it through users: each seeds 3 to 8 line items drawn from
# a 16 wide window, so draws collide and it lands on about 4.71 distinct keys per
# user. valsize is sized so the top of its sweep is the 10 GiB point, since items
# is fixed across a sweep and the smaller record sizes are therefore smaller
# datasets.
sizing_for() {
  case "$1" in
    mixed|scan|batch) echo "--items 2621440" ;;
    cart)             echo "--items 500000 --users 556570" ;;
    valsize)          echo "--items 40960" ;;
    *)                echo "" ;;
  esac
}

free_gib() {
  df -BG --output=avail "$DATA" | tail -1 | tr -dc '0-9'
}

run_point() {
  local engine=$1 workload=$2
  local sizing
  sizing=$(sizing_for "$workload")

  local avail
  avail=$(free_gib)
  if [ "$avail" -lt "$MIN_FREE_GIB" ]; then
    echo "SKIP $engine/$workload: only ${avail}GiB free under $DATA, need $MIN_FREE_GIB" \
      | tee -a "$MASTER_LOG"
    return 1
  fi

  echo "--- $engine / $workload  (${avail}GiB free)  $(date -Is) ---" | tee -a "$MASTER_LOG"
  local start=$SECONDS

  # shellcheck disable=SC2086
  taskset -c "$PCORES" ./keybench --config "$CONF" \
             --backend "$engine" \
             --report-dir "$RESULTS/$engine-$workload" \
             $sizing $KB_EXTRA \
             "workloads/$workload.lua" 2>&1 | tee -a "$MASTER_LOG"
  local rc=${PIPESTATUS[0]}

  echo "--- $engine / $workload done rc=$rc in $((SECONDS - start))s ---" | tee -a "$MASTER_LOG"

  # keybench destroys its own store on close, but a run that died leaves one
  # behind and the next point would seed onto a full disk.
  rm -rf "${DATA:?}"/keybench-*
  return $rc
}

WORKLOADS=${WORKLOADS:-"mixed cart scan batch valsize"}
ENGINES=${ENGINES:-"rocksdb tidesdb"}

for engine in $ENGINES; do
  for workload in $WORKLOADS; do
    run_point "$engine" "$workload"
  done
done

echo "=== runs finished $(date -Is), plotting ===" | tee -a "$MASTER_LOG"

# One plot.py invocation over every result directory, so a comparison figure
# carries both engines rather than one per engine. Plotting is pushed onto the
# E-cores: it is matplotlib rather than benchmark work, and there is no reason
# for it to touch the cores the run was measured on.
shopt -s nullglob
# --report-dir stamps a timestamped directory inside the one it is given, and the
# tsv files live in that inner directory, so the glob has to reach it. Pointing
# plot.py at the outer directory finds no rows and it writes nothing at all.
DIRS=("$RESULTS"/*-*/*/)
if [ ${#DIRS[@]} -eq 0 ]; then
  echo "no result directories to plot" | tee -a "$MASTER_LOG"
else
  taskset -c 16-31 python3 scripts/plot.py "${DIRS[@]}" --out "$RESULTS/figures" \
    2>&1 | tee -a "$MASTER_LOG"

  # plot.py's throughput_compare uses the peak thread count present in the data.
  # That is the right default for a sweep, but it makes the headline bar chart a
  # 16 thread comparison, so a single thread copy is written alongside it. The
  # rows are filtered into a scratch tree rather than by touching plot.py.
  ONE="$RESULTS/_1thread"
  rm -rf "$ONE"
  python3 - "$RESULTS" "$ONE" <<'PY'
import sys, os, glob
src_root, out_root = sys.argv[1], sys.argv[2]
for d in sorted(glob.glob(os.path.join(src_root, '*-*', '*', ''))):
    parts = d.rstrip(os.sep).split(os.sep)
    name = '-'.join(parts[-2:])
    dst = os.path.join(out_root, name)
    for fn in ('points.tsv', 'timeline.tsv'):
        s = os.path.join(d, fn)
        if not os.path.exists(s):
            continue
        with open(s) as fh:
            lines = fh.readlines()
        if not lines:
            continue
        cols = lines[0].rstrip('\n').split('\t')
        if 'threads' not in cols:
            continue
        i = cols.index('threads')
        keep = [L for L in lines[1:] if L.split('\t')[i] == '1']
        os.makedirs(dst, exist_ok=True)
        with open(os.path.join(dst, fn), 'w') as fo:
            fo.write(lines[0]); fo.writelines(keep)
PY
  DIRS1=("$ONE"/*/)
  if [ ${#DIRS1[@]} -gt 0 ]; then
    taskset -c 16-31 python3 scripts/plot.py "${DIRS1[@]}" \
      --out "$RESULTS/figures-1thread" 2>&1 | tee -a "$MASTER_LOG"
  fi
fi

echo "=== run-10gb-pinned finished $(date -Is) ===" | tee -a "$MASTER_LOG"
echo "results under $RESULTS/"
echo "  figures:          $RESULTS/figures/"
echo "  figures (1thread): $RESULTS/figures-1thread/"
