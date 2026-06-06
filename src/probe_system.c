#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "probe.h"

static int proc_field(const char *path, const char *key, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    int found = -1;
    size_t klen = strlen(key);
    while (fgets(line, sizeof line, f))
    {
        if (strncmp(line, key, klen) == 0)
        {
            char *c = strchr(line, ':');
            if (c)
            {
                c++;
                while (*c == ' ' || *c == '\t') c++;
                char *nl = strchr(c, '\n');
                if (nl) *nl = '\0';
                snprintf(out, n, "%s", c);
                found = 0;
            }
            break;
        }
    }
    fclose(f);
    return found;
}

static void sys_info(probe_info_cb cb, void *arg)
{
    char host[256];
    if (gethostname(host, sizeof host) == 0)
    {
        host[sizeof host - 1] = '\0';
        cb(arg, "host", host);
    }

    struct utsname u;
    if (uname(&u) == 0)
    {
        char os[256];
        snprintf(os, sizeof os, "%s %s %s", u.sysname, u.release, u.machine);
        cb(arg, "os", os);
    }

    char model[256];
    if (proc_field("/proc/cpuinfo", "model name", model, sizeof model) == 0) cb(arg, "cpu", model);
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores > 0)
    {
        char b[32];
        snprintf(b, sizeof b, "%ld", cores);
        cb(arg, "cpu_cores", b);
    }

    long psz = sysconf(_SC_PAGE_SIZE), pages = sysconf(_SC_PHYS_PAGES);
    if (pages > 0 && psz > 0)
    {
        char b[32];
        snprintf(b, sizeof b, "%.1f GiB", (double)pages * psz / (1024.0 * 1024 * 1024));
        cb(arg, "ram", b);
    }
    if (psz > 0)
    {
        char b[32];
        snprintf(b, sizeof b, "%ld", psz);
        cb(arg, "page_size", b);
    }

    const char *dir = probe_data_dir();
    struct statvfs vfs;
    if (statvfs(dir, &vfs) == 0)
    {
        char b[64], label[128];
        double tot = (double)vfs.f_blocks * vfs.f_frsize / (1024.0 * 1024 * 1024);
        double freeb = (double)vfs.f_bavail * vfs.f_frsize / (1024.0 * 1024 * 1024);
        snprintf(b, sizeof b, "%.1f GiB free of %.1f GiB", freeb, tot);
        snprintf(label, sizeof label, "data_fs(%s)", dir);
        cb(arg, label, b);
    }
}

static void sys_sample(probe_metric_cb cb, void *arg)
{
    static unsigned long long prev_idle, prev_total;
    FILE *f = fopen("/proc/stat", "r");
    if (f)
    {
        unsigned long long u, n, s, idle, io, irq, sirq, st;
        if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &s, &idle, &io, &irq,
                   &sirq, &st) >= 4)
        {
            unsigned long long total = u + n + s + idle + io + irq + sirq + st;
            unsigned long long dt = total - prev_total, di = idle - prev_idle;
            if (prev_total && dt) cb(arg, "cpu_util_pct", 100.0 * (double)(dt - di) / (double)dt);
            prev_total = total;
            prev_idle = idle;
        }
        fclose(f);
    }
    char avail[64];
    if (proc_field("/proc/meminfo", "MemAvailable", avail, sizeof avail) == 0)
        cb(arg, "mem_avail_mb", strtod(avail, NULL) / 1024.0);
    double load[1];
    if (getloadavg(load, 1) == 1) cb(arg, "load1", load[0]);

    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0)
    {
        double cpu = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec +
                     ru.ru_stime.tv_usec / 1e6;
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        static double prev_cpu = -1;
        static struct timespec prev_ts;
        if (prev_cpu >= 0)
        {
            double dw = (ts.tv_sec - prev_ts.tv_sec) + (ts.tv_nsec - prev_ts.tv_nsec) / 1e9;
            if (dw > 0) cb(arg, "proc_cpu_pct", 100.0 * (cpu - prev_cpu) / dw);
        }
        prev_cpu = cpu;
        prev_ts = ts;

        cb(arg, "peak_rss_mb", ru.ru_maxrss / 1024.0);
        cb(arg, "minflt", (double)ru.ru_minflt);
        cb(arg, "majflt", (double)ru.ru_majflt);
        cb(arg, "vol_ctxsw", (double)ru.ru_nvcsw);
        cb(arg, "invol_ctxsw", (double)ru.ru_nivcsw);
    }

    FILE *sm = fopen("/proc/self/statm", "r");
    if (sm)
    {
        long total_pages, res_pages;
        if (fscanf(sm, "%ld %ld", &total_pages, &res_pages) == 2)
            cb(arg, "rss_mb", (double)res_pages * sysconf(_SC_PAGE_SIZE) / (1024.0 * 1024));
        fclose(sm);
    }
    char b[64];
    if (proc_field("/proc/self/io", "read_bytes", b, sizeof b) == 0)
        cb(arg, "io_read_mb", strtod(b, NULL) / (1024.0 * 1024));
    if (proc_field("/proc/self/io", "write_bytes", b, sizeof b) == 0)
        cb(arg, "io_write_mb", strtod(b, NULL) / (1024.0 * 1024));
    if (proc_field("/proc/self/status", "Threads", b, sizeof b) == 0)
        cb(arg, "threads", strtod(b, NULL));
}

static const probe SYSTEM_PROBE = {"system", sys_info, sys_sample};
KV_REGISTER_PROBE(SYSTEM_PROBE);
