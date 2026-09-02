/* sched_setaffinity and the CPU_* macros are GNU extensions, and
   pthread_setaffinity_np is the per-thread form, so this file asks for the GNU
   surface rather than the POSIX one the rest of the harness compiles against. */
#define _GNU_SOURCE
#include "affinity.h"

#include <stdio.h>
#include <string.h>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#define KB_HAVE_AFFINITY 1
#endif

static int kb_enabled = 0;
static int kb_ncpus = 0;
static char kb_desc[160] = "unavailable";

#ifdef KB_HAVE_AFFINITY
/* The set is enumerated once at startup and then read by every worker, so the
   list is fixed for the run and needs no locking. CPU_SETSIZE bounds a cpu_set_t,
   which is 1024 CPUs -- past that sched_getaffinity would need CPU_ALLOC, and a
   machine that large is outside what this harness is for. */
static int kb_cpus[CPU_SETSIZE];

/* Render the set as ranges, so a contiguous 0..15 reads as "0-15" rather than
   sixteen numbers, and a fragmented allowance stays legible. */
static void kb_format_set(char *out, size_t n)
{
    size_t used = 0;
    out[0] = '\0';
    for (int i = 0; i < kb_ncpus;)
    {
        int j = i;
        while (j + 1 < kb_ncpus && kb_cpus[j + 1] == kb_cpus[j] + 1) j++;
        int w;
        if (j > i)
            w = snprintf(out + used, n - used, "%s%d-%d", used ? "," : "", kb_cpus[i], kb_cpus[j]);
        else
            w = snprintf(out + used, n - used, "%s%d", used ? "," : "", kb_cpus[i]);
        if (w < 0 || (size_t)w >= n - used)
        {
            snprintf(out + used, n - used, "...");
            return;
        }
        used += (size_t)w;
        i = j + 1;
    }
}
#endif

int kb_affinity_init(int enable)
{
#ifdef KB_HAVE_AFFINITY
    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (sched_getaffinity(0, sizeof mask, &mask) != 0)
    {
        kb_ncpus = 0;
        kb_enabled = 0;
        snprintf(kb_desc, sizeof kb_desc, "unavailable (sched_getaffinity failed)");
        return 0;
    }

    /* CPU_COUNT is the size of the allowance; the loop records which CPUs those
       are, since the set need not be contiguous or start at zero. */
    const int want = CPU_COUNT(&mask);
    kb_ncpus = 0;
    for (int i = 0; i < CPU_SETSIZE && kb_ncpus < want; i++)
        if (CPU_ISSET(i, &mask)) kb_cpus[kb_ncpus++] = i;

    kb_enabled = (enable && kb_ncpus > 0) ? 1 : 0;

    char set[96];
    kb_format_set(set, sizeof set);
    if (kb_enabled)
        snprintf(kb_desc, sizeof kb_desc, "on (%d cpu%s: %s)", kb_ncpus,
                 kb_ncpus == 1 ? "" : "s", set);
    else
        snprintf(kb_desc, sizeof kb_desc, "off (%d cpu%s available: %s)", kb_ncpus,
                 kb_ncpus == 1 ? "" : "s", set);
    return kb_ncpus;
#else
    (void)enable;
    kb_enabled = 0;
    kb_ncpus = 0;
    snprintf(kb_desc, sizeof kb_desc, "unavailable (not linux)");
    return 0;
#endif
}

int kb_affinity_cpus(void)
{
    return kb_ncpus;
}

int kb_pin_self(int tid)
{
#ifdef KB_HAVE_AFFINITY
    if (!kb_enabled || kb_ncpus <= 0) return -1;
    /* Wrap rather than refuse when threads outnumber CPUs: oversubscription is a
       case worth measuring, and this makes it an even, repeatable stacking. */
    const int cpu = kb_cpus[((tid % kb_ncpus) + kb_ncpus) % kb_ncpus];
    cpu_set_t one;
    CPU_ZERO(&one);
    CPU_SET(cpu, &one);
    if (pthread_setaffinity_np(pthread_self(), sizeof one, &one) != 0) return -1;
    return cpu;
#else
    (void)tid;
    return -1;
#endif
}

const char *kb_affinity_desc(void)
{
    return kb_desc;
}
