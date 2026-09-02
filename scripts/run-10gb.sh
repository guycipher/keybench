#!/bin/bash
# Drive the 10 GiB rocksdb vs tidesdb comparison across every bundled workload.
#
# One keybench invocation per engine and workload, rocksdb first and tidesdb
# after, because each workload reaches 10 GiB through a different knob and a
# config file carries only one --items and one --users. samples/rocksdb-vs-
# tidesdb-10gb.cnf supplies everything that is common to all of them.
#
# Every workload stores 4096 byte values, so 10 GiB is 2621440 records:
#
#   mixed scan batch   items 2621440              2621440 x 4K = 10 GiB
#   cart               users 556570               each user seeds 3 to 8 line
#                                                 items drawn from a 16 wide
#                                                 window, which lands on about
#                                                 4.71 distinct keys per user, so
#                                                 556570 x 4.71 x 4K = 10 GiB
#   valsize            items 40960                sized so the top of its value
#                                                 size sweep is 10 GiB. items is
#                                                 fixed across a sweep, so the
#                                                 smaller points are smaller
#                                                 datasets: 256B is 10 MiB, 4K is
#                                                 160 MiB, 64K is 2.5 GiB, and
#                                                 256K is the 10 GiB point.
#
# Results land in one directory per engine and workload under results/, each
# holding the console report, the points tsv, the timeline tsv, a replay config,
# and the plotted figures.

set -uo pipefail

KB_DIR=${KB_DIR:-/home/agpmastersystem/keybench}
CONF=${CONF:-$KB_DIR/samples/rocksdb-vs-tidesdb-10gb.cnf}
RESULTS=${RESULTS:-$KB_DIR/results}
DATA=${DATA:-/media/agpmastersystem/c794105c-0cd9-4be9-8369-ee6d6e707d68/home/development/benchmark/benchdata}

# A 10 GiB store plus the transient a compaction doubles it to needs headroom.
# Refuse to start a point that could fill the disk mid run rather than fail deep
# into a seed.
MIN_FREE_GIB=${MIN_FREE_GIB:-30}

# Extra flags appended to every invocation, which override the config file the
# way any flag does. Use it to rehearse the whole matrix cheaply before
# committing to the real thing:
#   KB_EXTRA="--items 50000 --users 20000 --secs 2 --threads 2" scripts/run-10gb.sh
KB_EXTRA=${KB_EXTRA:-}

cd "$KB_DIR" || exit 1
mkdir -p "$RESULTS" "$DATA" || exit 1

MASTER_LOG="$RESULTS/run-10gb.log"
echo "=== run-10gb started $(date -Is) ===" | tee -a "$MASTER_LOG"

# The sizing knobs each workload needs to reach 10 GiB.
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
  ./keybench --config "$CONF" \
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

echo "=== run-10gb finished $(date -Is) ===" | tee -a "$MASTER_LOG"
echo "results under $RESULTS/"
