#define _DEFAULT_SOURCE
#include <ctype.h>
#include <limits.h>
#include <mntent.h>
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

/* Find the mount that backs dir by taking the longest mount point that is a
   prefix of its real path, and report that device, filesystem type, and mount
   point. Returns 0 on success. */
static int data_mount(const char *dir, char *dev, size_t devn, char *fstype, size_t fsn, char *mnt,
                      size_t mntn)
{
    char rp[PATH_MAX];
    if (!realpath(dir, rp)) snprintf(rp, sizeof rp, "%s", dir);
    FILE *m = setmntent("/proc/mounts", "r");
    if (!m) return -1;
    struct mntent *e;
    size_t best = 0;
    int found = -1;
    while ((e = getmntent(m)) != NULL)
    {
        size_t l = strlen(e->mnt_dir);
        if (l >= best && strncmp(rp, e->mnt_dir, l) == 0 &&
            (l == 1 || rp[l] == '/' || rp[l] == '\0'))
        {
            best = l;
            snprintf(dev, devn, "%s", e->mnt_fsname);
            snprintf(fstype, fsn, "%s", e->mnt_type);
            snprintf(mnt, mntn, "%s", e->mnt_dir);
            found = 0;
        }
    }
    endmntent(m);
    return found;
}

/* Reduce a partition device path to the parent block device name used under
   /sys/block, so /dev/nvme0n1p2 becomes nvme0n1 and /dev/sda3 becomes sda. */
static void base_block(const char *dev, char *out, size_t n)
{
    const char *p = dev;
    if (strncmp(p, "/dev/", 5) == 0) p += 5;
    snprintf(out, n, "%s", p);
    if (strncmp(out, "nvme", 4) == 0 || strncmp(out, "mmcblk", 6) == 0)
    {
        char *pp = strrchr(out, 'p');
        if (pp && isdigit((unsigned char)pp[1])) *pp = '\0';
        return;
    }
    size_t len = strlen(out);
    while (len > 0 && isdigit((unsigned char)out[len - 1])) out[--len] = '\0';
}

static void sys_disk(const char *dir, probe_info_cb cb, void *arg)
{
    char dev[128], fstype[32], mnt[256];
    if (data_mount(dir, dev, sizeof dev, fstype, sizeof fstype, mnt, sizeof mnt) != 0) return;
    char b[512];
    snprintf(b, sizeof b, "%s on %s (%s)", dev, mnt, fstype);
    cb(arg, "data_dev", b);

    char base[128], path[256], line[128];
    base_block(dev, base, sizeof base);
    snprintf(path, sizeof path, "/sys/block/%s/queue/rotational", base);
    FILE *rf = fopen(path, "r");
    if (rf)
    {
        if (fgets(line, sizeof line, rf))
            cb(arg, "data_disk", atoi(line) ? "rotational (HDD)" : "solid state");
        fclose(rf);
    }
    snprintf(path, sizeof path, "/sys/block/%s/device/model", base);
    FILE *mf = fopen(path, "r");
    if (mf)
    {
        if (fgets(line, sizeof line, mf))
        {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            size_t l = strlen(line);
            while (l > 0 && line[l - 1] == ' ') line[--l] = '\0';
            if (l > 0) cb(arg, "data_disk_model", line);
        }
        fclose(mf);
    }
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
    sys_disk(dir, cb, arg);
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
