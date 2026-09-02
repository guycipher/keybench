#ifndef AFFINITY_H
#define AFFINITY_H

/* Worker CPU pinning.
 *
 * By default the harness leaves placement to the scheduler, which is what a real
 * service gets and so is the honest default. It also lets a worker migrate
 * mid-run, and a migration costs its warm caches, which is one of the things that
 * turns a cell's throughput bimodal across otherwise identical repeats. Pinning
 * removes that variable so a spread can be attributed to the engine rather than
 * to placement.
 *
 * The set is the one the process inherited rather than every CPU on the machine,
 * so a run under taskset, a cpuset cgroup, or a container CPU limit pins inside
 * that allowance instead of escaping it. Worker t takes the t-th CPU of that set,
 * wrapping when there are more threads than CPUs -- so a 64 thread run on 16
 * allowed CPUs puts four workers on each, deliberately and reproducibly, rather
 * than wherever the scheduler drifts.
 */

/* Enumerate the inherited CPU set. enable of 0 records the set for reporting but
   leaves pinning off. Returns the number of CPUs in the set, or 0 if the platform
   does not support affinity, in which case pinning silently does nothing. */
int kb_affinity_init(int enable);

/* CPUs in the inherited set, 0 when unavailable. */
int kb_affinity_cpus(void);

/* Pin the calling thread to the tid-th CPU of the inherited set. Returns the CPU
   it pinned to, or -1 when pinning is off or unavailable. Call from inside the
   thread at entry, so the mask is set before any work is done. */
int kb_pin_self(int tid);

/* One line for the run report, e.g. "on (16 cpus: 0-15)" or "off (16 available)". */
const char *kb_affinity_desc(void);

#endif
