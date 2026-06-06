#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "backend_registry.h"
#include "bench.h"
#include "config.h"
#include "hist.h"
#include "kvlua.h"
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include "probe.h"
#include "reporter.h"
#include "store.h"

#ifndef KEYBENCH_VERSION
#define KEYBENCH_VERSION "dev"
#endif

#define DU_PATH_MAX    4096
#define DU_MAX_PENDING 64
#define DU_MAX_ENTRIES 2000000

/* Upper bound on metrics gathered in one timeline sample. Engines that expose a
   wide stat surface, tidesdb with per-level counters in particular, plus the
   system probe and throughput, must all fit without being dropped. */
#define SAMPLE_METRIC_MAX 256

static uint64_t du_bytes(const char *root)
{
    struct stat st;
    if (lstat(root, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return (uint64_t)st.st_size;

    char(*pending)[DU_PATH_MAX] = malloc((size_t)DU_MAX_PENDING * sizeof *pending);
    if (!pending) return (uint64_t)st.st_size;
    int sp = 0;
    uint64_t total = (uint64_t)st.st_size;
    snprintf(pending[sp++], DU_PATH_MAX, "%s", root);

    long guard = 0;
    while (sp > 0 && guard < DU_MAX_ENTRIES)
    {
        char dir[DU_PATH_MAX];
        snprintf(dir, sizeof dir, "%s", pending[--sp]);
        DIR *d = opendir(dir);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) && guard < DU_MAX_ENTRIES)
        {
            guard++;
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char child[DU_PATH_MAX];
            size_t dlen = strlen(dir), nlen = strlen(e->d_name);
            if (dlen + 1 + nlen >= sizeof child) continue;
            memcpy(child, dir, dlen);
            child[dlen] = '/';
            memcpy(child + dlen + 1, e->d_name, nlen + 1);
            if (lstat(child, &st) != 0) continue;
            total += (uint64_t)st.st_size;
            if (S_ISDIR(st.st_mode) && sp < DU_MAX_PENDING)
                snprintf(pending[sp++], DU_PATH_MAX, "%s", child);
        }
        closedir(d);
    }
    free(pending);
    return total;
}

uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

typedef struct
{
    long ops;
    double secs;
    long users;
    long items;
    unsigned seed;
    int threads;
    int batch;
    double timeline;
    const char *backend;
    const char *data_dir;
    const cfg_file *conf;
} config;

static void list_backends(void)
{
    for (int i = 0; i < backend_count(); i++)
        fprintf(stderr, "%s%s", i ? ", " : "", backend_at(i)->name);
}

static void fmt_ns(uint64_t ns, char *out, size_t n)
{
    if (ns < 1000ull)
        snprintf(out, n, "%lu ns", (unsigned long)ns);
    else if (ns < 1000000ull)
        snprintf(out, n, "%.2f us", ns / 1e3);
    else if (ns < 1000000000ull)
        snprintf(out, n, "%.2f ms", ns / 1e6);
    else
        snprintf(out, n, "%.2f s", ns / 1e9);
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double median_d(double *a, int n)
{
    if (n <= 0) return 0;
    qsort(a, n, sizeof *a, cmp_d);
    return (n & 1) ? a[n / 2] : 0.5 * (a[n / 2 - 1] + a[n / 2]);
}

static int make_ctx(lua_State *L, const config *c)
{
    lua_createtable(L, 0, 6);
    lua_pushinteger(L, c->users);
    lua_setfield(L, -2, "users");
    lua_pushinteger(L, c->items);
    lua_setfield(L, -2, "items");
    lua_pushinteger(L, c->ops);
    lua_setfield(L, -2, "ops");
    lua_pushinteger(L, c->seed);
    lua_setfield(L, -2, "seed");
    lua_pushinteger(L, c->batch);
    lua_setfield(L, -2, "batch");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "iter");
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

static lua_State *open_workload(const char *path, kvlua_ctx *kc, unsigned seed)
{
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    char seedcmd[64];
    snprintf(seedcmd, sizeof seedcmd, "math.randomseed(%u)", seed);
    (void)luaL_dostring(L, seedcmd);
    kvlua_register(L, kc);

    if (luaL_dofile(L, path) != LUA_OK)
    {
        fprintf(stderr, "error loading %s: %s\n", path, lua_tostring(L, -1));
        lua_close(L);
        return NULL;
    }
    if (!lua_istable(L, -1))
    {
        fprintf(stderr, "%s: workload must `return` a table\n", path);
        lua_close(L);
        return NULL;
    }
    return L;
}

typedef struct
{
    const char *path;
    const config *cfg;
    kv_store *store;
    long ops;
    unsigned seed;

    kvlua_ctx kc;
    long units;
    int err;
} worker;

static void *worker_main(void *arg)
{
    worker *w = arg;
    const config *cfg = w->cfg;
    w->kc.store = w->store;
    stats_reset(&w->kc.stats);

    lua_State *L = open_workload(w->path, &w->kc, w->seed);
    if (!L)
    {
        w->err = 1;
        return NULL;
    }
    int wl = luaL_ref(L, LUA_REGISTRYINDEX);
    int ctx = make_ctx(L, cfg);

    lua_rawgeti(L, LUA_REGISTRYINDEX, wl);
    lua_getfield(L, -1, "run");
    if (!lua_isfunction(L, -1))
    {
        fprintf(stderr, "%s: missing run() function\n", w->path);
        lua_pop(L, 2);
        w->err = 1;
        lua_close(L);
        return NULL;
    }
    int runfn = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);

    uint64_t t0 = now_ns();
    long i = 0;
    for (;;)
    {
        if (cfg->secs <= 0 && i >= w->ops) break;
        if (cfg->secs > 0 && (now_ns() - t0) >= (uint64_t)(cfg->secs * 1e9)) break;

        lua_rawgeti(L, LUA_REGISTRYINDEX, ctx);
        lua_pushinteger(L, i + 1);
        lua_setfield(L, -2, "iter");
        lua_pop(L, 1);
        lua_rawgeti(L, LUA_REGISTRYINDEX, runfn);
        lua_rawgeti(L, LUA_REGISTRYINDEX, ctx);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK)
        {
            fprintf(stderr, "run() error at iter %ld: %s\n", i, lua_tostring(L, -1));
            lua_pop(L, 1);
            w->err = 1;
            break;
        }
        i++;
        KV_RELAXED_SET(w->units, i);
    }
    KV_RELAXED_SET(w->units, i);
    lua_close(L);
    return NULL;
}

typedef struct
{
    worker *ws;
    int nthreads;
    kv_store *store;
    double interval;
    uint64_t t0;
    volatile int stop;
    reporter_set *reporters;
    const char *workload, *engine;
    int threads, batch;
} sampler;

typedef struct
{
    sample_metric *m;
    int *nm;
    int max;
} samp_collect;
static void samp_stat_cb(void *arg, const char *name, double v)
{
    samp_collect *c = arg;
    if (*c->nm < c->max)
    {
        c->m[*c->nm].name = name;
        c->m[*c->nm].value = v;
        (*c->nm)++;
    }
    else
    {
        static int warned = 0;
        if (!warned)
        {
            warned = 1;
            fprintf(stderr, "keybench: >%d sample metrics, extras dropped\n", c->max);
        }
    }
}

static void *sampler_main(void *arg)
{
    sampler *sp = arg;
    uint64_t prev_ops = 0;
    long prev_units = 0;
    uint64_t last = sp->t0;
    while (!sp->stop)
    {
        uint64_t target = last + (uint64_t)(sp->interval * 1e9);
        while (!sp->stop && now_ns() < target)
        {
            struct timespec ts = {0, 10 * 1000 * 1000};
            nanosleep(&ts, NULL);
        }
        if (sp->stop) break;
        uint64_t nowt = now_ns();
        double dt = (nowt - last) / 1e9;
        last = nowt;

        uint64_t cur_ops = 0;
        long cur_units = 0;
        for (int t = 0; t < sp->nthreads; t++)
        {
            cur_ops += KV_RELAXED_GET(sp->ws[t].kc.stats.prim_ops);
            cur_units += KV_RELAXED_GET(sp->ws[t].units);
        }
        sample_metric m[SAMPLE_METRIC_MAX];
        int nm = 0;
        m[nm++] = (sample_metric){"wu_per_s", dt > 0 ? (double)(cur_units - prev_units) / dt : 0};
        m[nm++] = (sample_metric){"ops_per_s", dt > 0 ? (double)(cur_ops - prev_ops) / dt : 0};
        prev_ops = cur_ops;
        prev_units = cur_units;

        samp_collect sc = {m, &nm, SAMPLE_METRIC_MAX};
        store_stats(sp->store, samp_stat_cb, &sc);
        for (int pi = 0; pi < probe_selected_count(); pi++)
        {
            const probe *pr = probe_selected_at(pi);
            if (pr->sample) pr->sample(samp_stat_cb, &sc);
        }
        const char *dd = store_datadir(sp->store);
        if (dd && nm < SAMPLE_METRIC_MAX)
        {
            m[nm].name = "disk_bytes";
            m[nm].value = (double)du_bytes(dd);
            nm++;
        }

        report_sample rs = {
            sp->workload, sp->engine, sp->threads, sp->batch, (double)(nowt - sp->t0) / 1e9, m, nm};
        reporters_sample(sp->reporters, &rs);
    }
    return NULL;
}

typedef struct
{
    op_stats stats;
    long units;
    double wall;
    const char *engine;
    int ok;
} point_result;

typedef struct
{
    worker *ws;
    int nthreads;
    long target;
    double secs;
    uint64_t t0;
    volatile int stop;
    const char *workload, *engine;
    int threads, batch;
} progress;

#define PROGRESS_TICK_NS 1000000000ull
#define PROGRESS_STEP_NS 20000000

/* Stream a one-line status to stderr while a point runs, so a long single-thread
   point looks alive rather than hung. Updates in place with a carriage return.
   stderr keeps it off the stdout report and tsv streams. */
static void *progress_main(void *arg)
{
    progress *p = arg;
    long prev_units = 0;
    uint64_t prev_t = p->t0;
    while (!p->stop)
    {
        uint64_t until = now_ns() + PROGRESS_TICK_NS;
        while (!p->stop && now_ns() < until)
        {
            struct timespec ts = {0, PROGRESS_STEP_NS};
            nanosleep(&ts, NULL);
        }
        if (p->stop) break;
        uint64_t now = now_ns();
        long units = 0;
        for (int t = 0; t < p->nthreads; t++) units += KV_RELAXED_GET(p->ws[t].units);
        double dt = (now - prev_t) / 1e9;
        double rate = dt > 0 ? (double)(units - prev_units) / dt : 0.0;
        double elapsed = (now - p->t0) / 1e9;
        prev_units = units;
        prev_t = now;
        if (p->secs > 0)
            fprintf(stderr, "\r  %s %s t%d b%d  %5.1f/%.0fs  %9.0f wu/s          ", p->workload,
                    p->engine, p->threads, p->batch, elapsed, p->secs, rate);
        else
            fprintf(stderr, "\r  %s %s t%d b%d  %5.1fs  %9.0f wu/s  %3.0f%% (%ld/%ld)     ",
                    p->workload, p->engine, p->threads, p->batch, elapsed, rate,
                    p->target > 0 ? 100.0 * (double)units / (double)p->target : 0.0, units,
                    p->target);
        fflush(stderr);
    }
    return NULL;
}

static void run_measurement(const char *path, const config *cfg, char *name_out, size_t name_sz,
                            reporter_set *reporters, point_result *R)
{
    stats_reset(&R->stats);
    R->units = 0;
    R->wall = 0;
    R->engine = NULL;
    R->ok = 0;

    const backend_entry *eng = backend_find(cfg->backend);
    if (!eng)
    {
        fprintf(stderr, "unknown backend '%s'\n", cfg->backend);
        return;
    }

    int nthreads = cfg->threads < 1 ? 1 : cfg->threads;

    const kv_options *opts = cfg_section(cfg->conf, eng->name);
    kv_store *store = store_open(eng->open, cfg->seed, cfg->data_dir, opts);
    R->engine = store_version(store);

    char name[128] = "(unnamed)";
    {
        kvlua_ctx kc = {.store = store};
        stats_reset(&kc.stats);
        lua_State *L = open_workload(path, &kc, cfg->seed);
        if (!L)
        {
            store_close(store);
            return;
        }

        lua_getfield(L, -1, "name");
        if (lua_isstring(L, -1)) snprintf(name, sizeof name, "%s", lua_tostring(L, -1));
        lua_pop(L, 1);

        lua_getfield(L, -1, "load");
        if (lua_isfunction(L, -1))
        {
            int show = isatty(STDERR_FILENO);
            uint64_t ls = now_ns();
            if (show)
            {
                fprintf(stderr, "  seeding %s on %s ...", name, R->engine);
                fflush(stderr);
            }
            int ctx = make_ctx(L, cfg);
            lua_rawgeti(L, LUA_REGISTRYINDEX, ctx);
            if (lua_pcall(L, 1, 0, 0) != LUA_OK)
            {
                fprintf(stderr, "\n%s load() error: %s\n", name, lua_tostring(L, -1));
                lua_close(L);
                store_close(store);
                return;
            }
            if (show) fprintf(stderr, " done (%.1fs)\n", (now_ns() - ls) / 1e9);
        }
        else
        {
            lua_pop(L, 1);
        }
        lua_close(L);
    }
    if (name_out) snprintf(name_out, name_sz, "%s", name);

    worker *ws = calloc((size_t)nthreads, sizeof *ws);
    pthread_t *tids = calloc((size_t)nthreads, sizeof *tids);
    if (!ws || !tids)
    {
        fprintf(stderr, "keybench: out of memory\n");
        abort();
    }

    long base = cfg->secs > 0 ? 0 : cfg->ops / nthreads;
    long rem = cfg->secs > 0 ? 0 : cfg->ops % nthreads;
    for (int t = 0; t < nthreads; t++)
    {
        ws[t].path = path;
        ws[t].cfg = cfg;
        ws[t].store = store;
        ws[t].ops = base + (t < rem ? 1 : 0);
        ws[t].seed = cfg->seed + (unsigned)t;
    }

    uint64_t t0 = now_ns();
    for (int t = 0; t < nthreads; t++) pthread_create(&tids[t], NULL, worker_main, &ws[t]);

    int show_progress = isatty(STDERR_FILENO);
    progress pr;
    pthread_t prtid;
    if (show_progress)
    {
        pr = (progress){.ws = ws,
                        .nthreads = nthreads,
                        .target = cfg->ops,
                        .secs = cfg->secs,
                        .t0 = t0,
                        .stop = 0,
                        .workload = name_out ? name_out : name,
                        .engine = R->engine,
                        .threads = nthreads,
                        .batch = cfg->batch};
        pthread_create(&prtid, NULL, progress_main, &pr);
    }

    int sampling = cfg->timeline > 0 && reporters && reporters_want_samples(reporters);
    sampler sp;
    pthread_t sptid;
    if (sampling)
    {
        sp = (sampler){.ws = ws,
                       .nthreads = nthreads,
                       .store = store,
                       .interval = cfg->timeline,
                       .t0 = t0,
                       .stop = 0,
                       .reporters = reporters,
                       .workload = name_out ? name_out : name,
                       .engine = R->engine,
                       .threads = nthreads,
                       .batch = cfg->batch};
        pthread_create(&sptid, NULL, sampler_main, &sp);
    }

    for (int t = 0; t < nthreads; t++) pthread_join(tids[t], NULL);
    R->wall = (now_ns() - t0) / 1e9;

    if (sampling)
    {
        sp.stop = 1;
        pthread_join(sptid, NULL);
    }
    if (show_progress)
    {
        pr.stop = 1;
        pthread_join(prtid, NULL);
        fprintf(stderr, "\r%78s\r", ""); /* erase the live line before the report */
    }

    int any_err = 0;
    for (int t = 0; t < nthreads; t++)
    {
        stats_merge(&R->stats, &ws[t].kc.stats);
        R->units += ws[t].units;
        if (ws[t].err) any_err = 1;
    }
    R->ok = !any_err;

    free(ws);
    free(tids);
    store_close(store);
}

typedef struct
{
    const char *engine;
    int threads, batch;
    double wu_s, ops_s, p50, p99;
} summary_row;

static summary_row summarize_point(const char *name, point_result *Rs, int nr, int threads,
                                   int batch, reporter_set *reporters)
{
    summary_row row = {.engine = Rs[0].engine, .threads = threads, .batch = batch};
    double *tmp = malloc((size_t)nr * sizeof *tmp);
    report_point rp = {0};
    rp.workload = name;
    rp.engine = Rs[0].engine;
    rp.threads = threads;
    rp.batch = batch;
    rp.repeat = nr;

    for (int i = 0; i < OP__COUNT && rp.n_ops < REPORT_MAX_OPS; i++)
    {
        uint64_t anyc = 0;
        for (int r = 0; r < nr; r++) anyc += Rs[r].stats.h[i].count;
        if (anyc == 0) continue;
        report_op *o = &rp.ops[rp.n_ops++];
        o->op = op_name(i);
        for (int r = 0; r < nr; r++) tmp[r] = (double)Rs[r].stats.h[i].count;
        o->count = (uint64_t)(median_d(tmp, nr) + 0.5);
        for (int r = 0; r < nr; r++) tmp[r] = (double)hist_percentile(&Rs[r].stats.h[i], 50);
        o->p50 = (uint64_t)median_d(tmp, nr);
        for (int r = 0; r < nr; r++) tmp[r] = (double)hist_percentile(&Rs[r].stats.h[i], 99);
        o->p99 = (uint64_t)median_d(tmp, nr);
        for (int r = 0; r < nr; r++) tmp[r] = (double)hist_percentile(&Rs[r].stats.h[i], 99.9);
        o->p999 = (uint64_t)median_d(tmp, nr);
        for (int r = 0; r < nr; r++) tmp[r] = (double)Rs[r].stats.h[i].max;
        o->max = (uint64_t)median_d(tmp, nr);
    }

    for (int r = 0; r < nr; r++)
        if (Rs[r].stats.get_hits + Rs[r].stats.get_misses > 0)
        {
            rp.has_hit = 1;
            break;
        }
    if (rp.has_hit)
    {
        for (int r = 0; r < nr; r++)
        {
            double tot = (double)(Rs[r].stats.get_hits + Rs[r].stats.get_misses);
            tmp[r] = tot > 0 ? 100.0 * Rs[r].stats.get_hits / tot : 0.0;
        }
        rp.hit_rate = median_d(tmp, nr);
    }

    for (int r = 0; r < nr; r++) tmp[r] = Rs[r].wall > 0 ? Rs[r].units / Rs[r].wall : 0.0;
    rp.wu_med = median_d(tmp, nr);
    rp.wu_lo = tmp[0];
    rp.wu_hi = tmp[nr - 1];
    for (int r = 0; r < nr; r++) tmp[r] = Rs[r].wall > 0 ? Rs[r].stats.prim_ops / Rs[r].wall : 0.0;
    rp.ops_med = median_d(tmp, nr);
    rp.ops_lo = tmp[0];
    rp.ops_hi = tmp[nr - 1];

    for (int r = 0; r < nr; r++)
    {
        hist all;
        hist_reset(&all);
        for (int i = 0; i < OP__COUNT; i++) hist_merge(&all, &Rs[r].stats.h[i]);
        tmp[r] = (double)hist_percentile(&all, 50);
    }
    rp.overall_p50 = (uint64_t)median_d(tmp, nr);
    for (int r = 0; r < nr; r++)
    {
        hist all;
        hist_reset(&all);
        for (int i = 0; i < OP__COUNT; i++) hist_merge(&all, &Rs[r].stats.h[i]);
        tmp[r] = (double)hist_percentile(&all, 99);
    }
    rp.overall_p99 = (uint64_t)median_d(tmp, nr);

    reporters_point(reporters, &rp);

    row.wu_s = rp.wu_med;
    row.ops_s = rp.ops_med;
    row.p50 = (double)rp.overall_p50;
    row.p99 = (double)rp.overall_p99;
    free(tmp);
    return row;
}

/* A workload opts into the batch sweep by returning batched = true in its table.
   Only mget/mput style workloads respond to ctx.batch; the rest do identical
   work at every batch size, so keybench runs them once at batch 1 rather than
   repeating the same measurement. The flag is read by loading the file with no
   store, since the top-level return touches no kv verbs. */
static int read_workload_batched(const char *path, unsigned seed)
{
    kvlua_ctx kc = {.store = NULL};
    stats_reset(&kc.stats);
    lua_State *L = open_workload(path, &kc, seed);
    if (!L) return 0;
    lua_getfield(L, -1, "batched");
    int batched = lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_close(L);
    return batched;
}

static void run_file(const char *path, const config *base, const char *const *engines, int neng,
                     const int *tlist, int ntp, const int *blist, int nbp, int repeat,
                     reporter_set *reporters)
{
    point_result *Rs = malloc((size_t)repeat * sizeof *Rs);
    char name[128] = "(unnamed)";
    int nrows = 0;

    static const int one_batch = 1;
    if (!read_workload_batched(path, base->seed) && nbp > 1)
    {
        fprintf(stderr, "%s: workload ignores ctx.batch, running batch=1 only\n", path);
        blist = &one_batch;
        nbp = 1;
    }
    int npoints = neng * ntp * nbp;
    summary_row *rows = malloc((size_t)npoints * sizeof *rows);

    for (int ei = 0; ei < neng; ei++)
    {
        for (int ti = 0; ti < ntp; ti++)
        {
            for (int bi = 0; bi < nbp; bi++)
            {
                config pc = *base;
                pc.backend = engines[ei];
                pc.threads = tlist[ti];
                pc.batch = blist[bi];
                int ok = 1;
                for (int r = 0; r < repeat; r++)
                {
                    run_measurement(path, &pc, name, sizeof name, reporters, &Rs[r]);
                    if (!Rs[r].ok)
                    {
                        ok = 0;
                        break;
                    }
                }
                if (!ok) continue;
                rows[nrows++] = summarize_point(name, Rs, repeat, pc.threads, pc.batch, reporters);
            }
        }
    }

    if (npoints > 1 && nrows > 0)
    {
        printf("\n%s: sweep summary (medians)\n", name);
        printf("%-12s %8s %6s %14s %14s %10s %10s\n", "engine", "threads", "batch", "wu/sec",
               "ops/sec", "p50", "p99");
        char p50[32], p99[32];
        for (int i = 0; i < nrows; i++)
        {
            fmt_ns((uint64_t)rows[i].p50, p50, sizeof p50);
            fmt_ns((uint64_t)rows[i].p99, p99, sizeof p99);
            printf("%-12s %8d %6d %14.0f %14.0f %10s %10s\n", rows[i].engine, rows[i].threads,
                   rows[i].batch, rows[i].wu_s, rows[i].ops_s, p50, p99);
        }
    }

    free(rows);
    free(Rs);
}

static long arg_long(const char *opt, const char *s)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno != 0)
    {
        fprintf(stderr, "keybench: invalid number for %s '%s'\n", opt, s);
        exit(2);
    }
    return v;
}

static double arg_double(const char *opt, const char *s)
{
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (end == s || *end != '\0' || errno != 0)
    {
        fprintf(stderr, "keybench: invalid number for %s '%s'\n", opt, s);
        exit(2);
    }
    return v;
}

static int parse_int_list(const char *s, int *out, int max)
{
    int n = 0;
    while (*s && n < max)
    {
        char *end;
        long v = strtol(s, &end, 10);
        if (end == s) break;
        out[n++] = v < 1 ? 1 : (int)v;
        s = end;
        if (*s == ',') s++;
    }
    return n;
}

static void apply_config(const cfg_file *conf, config *cfg, const char **threads_arg,
                         const char **batch_arg, const char **report_arg, int *repeat,
                         const char **cfiles, int *ncfiles, int maxfiles)
{
    const kv_options *b = cfg_section(conf, "bench");
    if (!b) return;
    const char *s;
    if ((s = kv_opt_str(b, "backend", NULL))) cfg->backend = s;
    if ((s = kv_opt_str(b, "threads", NULL))) *threads_arg = s;
    if ((s = kv_opt_str(b, "batch", NULL))) *batch_arg = s;
    if ((s = kv_opt_str(b, "report", NULL))) *report_arg = s;
    if ((s = kv_opt_str(b, "secs", NULL))) cfg->secs = atof(s);
    if ((s = kv_opt_str(b, "timeline", NULL))) cfg->timeline = atof(s);
    if ((s = kv_opt_str(b, "data_dir", NULL))) cfg->data_dir = s;
    cfg->ops = kv_opt_int(b, "ops", cfg->ops);
    cfg->users = kv_opt_int(b, "users", cfg->users);
    cfg->items = kv_opt_int(b, "items", cfg->items);
    cfg->seed = (unsigned)kv_opt_int(b, "seed", cfg->seed);
    *repeat = (int)kv_opt_int(b, "repeat", *repeat);
    *ncfiles = kv_opt_all(b, "workload", cfiles, maxfiles);
}

static void save_config(const char *path, const config *cfg, const char *engines_str,
                        const char *threads_list, const char *batch_list, int repeat,
                        const char *report_arg, const char *report_dir, const char *const *files,
                        int nfiles, const cfg_file *conf)
{
    FILE *f = fopen(path, "w");
    if (!f)
    {
        fprintf(stderr, "cannot write replay file '%s'\n", path);
        return;
    }
    fprintf(f, "keybench %s replay file - reproduce with:  keybench --config %s\n\n",
            KEYBENCH_VERSION, path);
    fprintf(f, "[bench]\n");
    fprintf(f, "backend = %s\n", engines_str);
    fprintf(f, "threads = %s\n", threads_list);
    fprintf(f, "batch   = %s\n", batch_list);
    if (cfg->secs > 0)
        fprintf(f, "secs = %g\n", cfg->secs);
    else
        fprintf(f, "ops  = %ld\n", cfg->ops);
    fprintf(f, "users  = %ld\n", cfg->users);
    fprintf(f, "items  = %ld\n", cfg->items);
    fprintf(f, "seed   = %u\n", cfg->seed);
    fprintf(f, "repeat = %d\n", repeat);
    if (report_dir)
        fprintf(f, "report_dir = %s\n", report_dir);
    else if (report_arg)
        fprintf(f, "report = %s\n", report_arg);
    for (int i = 0; i < nfiles; i++) fprintf(f, "workload = %s\n", files[i]);
    for (int i = 0; conf && i < cfg_nsections(conf); i++)
    {
        const kv_options *o = cfg_section_at(conf, i);
        if (!strcmp(kv_opt_name(o), "bench")) continue;
        fprintf(f, "\n[%s]\n", kv_opt_name(o));
        for (int j = 0; j < kv_opt_count(o); j++)
            fprintf(f, "%s = %s\n", kv_opt_key_at(o, j), kv_opt_val_at(o, j));
    }
    fclose(f);
    fprintf(stderr, "wrote replay file %s\n", path);
}

struct pinfo
{
    char *k[32], *v[32];
    int n;
};
static void pinfo_cb(void *arg, const char *k, const char *v)
{
    struct pinfo *c = arg;
    if (c->n < 32)
    {
        c->k[c->n] = strdup(k);
        c->v[c->n] = strdup(v);
        c->n++;
    }
}
static void emit_probes(reporter_set *reporters)
{
    int rendered = reporters_want_probe(reporters);
    for (int i = 0; i < probe_selected_count(); i++)
    {
        const probe *p = probe_selected_at(i);
        if (!p->info) continue;
        struct pinfo pi = {.n = 0};
        p->info(pinfo_cb, &pi);
        if (rendered)
        {
            probe_kv kv[32];
            for (int j = 0; j < pi.n; j++)
            {
                kv[j].key = pi.k[j];
                kv[j].value = pi.v[j];
            }
            report_probe rp = {p->name, kv, pi.n};
            reporters_probe(reporters, &rp);
        }
        else
        {
            fprintf(stderr, "[probe: %s]\n", p->name);
            for (int j = 0; j < pi.n; j++) fprintf(stderr, "  %-14s %s\n", pi.k[j], pi.v[j]);
        }
        for (int j = 0; j < pi.n; j++)
        {
            free(pi.k[j]);
            free(pi.v[j]);
        }
    }
}

/* Create base/<timestamp>/ and point the reporters and replay file at it, so a
 * run's artifacts (human report, points.tsv, timeline.tsv, replay.cnf) land in
 * one self-contained directory. Builds the report spec and replay path into the
 * caller's buffers. Returns 0 on success. */
static int setup_report_dir(const char *base, char *spec, size_t spec_n, char *replay,
                            size_t replay_n)
{
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char ts[32];
    strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &tmv);

    char dir[600];
    snprintf(dir, sizeof dir, "%s/%s", base, ts);
    mkdir(base, 0755); /* base may already exist; the leaf must be fresh */
    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
    {
        fprintf(stderr, "cannot create report dir '%s': %s\n", dir, strerror(errno));
        return -1;
    }
    snprintf(spec, spec_n,
             "console,console:%s/report.txt,tsv:%s/points.tsv,timeline:%s/timeline.tsv", dir, dir,
             dir);
    snprintf(replay, replay_n, "%s/replay.cnf", dir);
    fprintf(stderr, "keybench: writing reports to %s/\n", dir);
    return 0;
}

static void usage(const char *p)
{
    fprintf(
        stderr,
        "usage: %s [options] WORKLOAD.lua [WORKLOAD2.lua ...]\n"
        "  --ops N      total workload units (run() calls) per workload, split across threads "
        "(default 200000)\n"
        "  --secs S     run each workload for S seconds instead of a fixed count\n"
        "  --threads L  worker-thread counts; a comma list sweeps (e.g. 1,2,4,8) (default 1)\n"
        "  --batch L    ctx.batch sizes for mget/mput; a comma list sweeps (e.g. 1,8,64) (default "
        "1)\n"
        "  --repeat N   run each (threads x batch) point N times and report the median (default "
        "1)\n"
        "  --report S   reporters, comma list of name[:path] (console, tsv, timeline); e.g. "
        "console,timeline:tl.tsv\n"
        "  --report-dir D  create D/<timestamp>/ and write report.txt, points.tsv, timeline.tsv, "
        "replay.cnf there\n"
        "  --timeline S sample interval (s) for real-time stats; needs a timeline reporter\n"
        "  --probe L    system probes to run (default all; 'none' to disable); e.g. system\n"
        "  --data-dir D base dir persistent engines store data under (default /tmp)\n"
        "  --users N    user population knob (default 100000)\n"
        "  --items N    catalog size knob (default 100000)\n"
        "  --seed N     RNG seed (default 1)\n"
        "  --backend L  engine(s); a comma list compares them (e.g. skiplist,rocksdb)\n"
        "  --config F   load a .cnf config (its [bench] section; [<engine>] tunes that engine)\n"
        "  --save-config F  write the effective run as a .cnf replay file (rerun with --config F)\n"
        "Run multiple files to run them all in sequence (each isolated).\n",
        p);
}

int main(int argc, char **argv)
{
    config cfg = {.ops = 200000,
                  .secs = 0,
                  .users = 100000,
                  .items = 100000,
                  .seed = 1,
                  .threads = 1,
                  .batch = 1,
                  .backend = NULL,
                  .data_dir = NULL,
                  .conf = NULL};
    const char *files[64];
    int nfiles = 0;
    const char *threads_arg = NULL, *batch_arg = NULL, *report_arg = NULL, *save_arg = NULL;
    const char *probe_arg = NULL, *report_dir_arg = NULL;
    int repeat = 1;

    cfg_file *conf = NULL;
    const char *cfiles[64];
    int ncfiles = 0;
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--config") && i + 1 < argc)
        {
            if (cfg_load(&conf, argv[i + 1]) != 0) return 2;
            i++;
        }
        else if (!strcmp(argv[i], "--save-config") && i + 1 < argc)
        {
            save_arg = argv[i + 1];
            i++;
        }
    }
    if (conf)
    {
        apply_config(conf, &cfg, &threads_arg, &batch_arg, &report_arg, &repeat, cfiles, &ncfiles,
                     64);
        cfg.conf = conf;
    }

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--ops") && i + 1 < argc)
            cfg.ops = arg_long("--ops", argv[++i]);
        else if (!strcmp(argv[i], "--secs") && i + 1 < argc)
            cfg.secs = arg_double("--secs", argv[++i]);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc)
            threads_arg = argv[++i];
        else if (!strcmp(argv[i], "--batch") && i + 1 < argc)
            batch_arg = argv[++i];
        else if (!strcmp(argv[i], "--repeat") && i + 1 < argc)
            repeat = (int)arg_long("--repeat", argv[++i]);
        else if (!strcmp(argv[i], "--report") && i + 1 < argc)
            report_arg = argv[++i];
        else if (!strcmp(argv[i], "--report-dir") && i + 1 < argc)
            report_dir_arg = argv[++i];
        else if (!strcmp(argv[i], "--timeline") && i + 1 < argc)
            cfg.timeline = arg_double("--timeline", argv[++i]);
        else if (!strcmp(argv[i], "--probe") && i + 1 < argc)
            probe_arg = argv[++i];
        else if (!strcmp(argv[i], "--data-dir") && i + 1 < argc)
            cfg.data_dir = argv[++i];
        else if (!strcmp(argv[i], "--users") && i + 1 < argc)
            cfg.users = arg_long("--users", argv[++i]);
        else if (!strcmp(argv[i], "--items") && i + 1 < argc)
            cfg.items = arg_long("--items", argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc)
            cfg.seed = (unsigned)arg_long("--seed", argv[++i]);
        else if (!strcmp(argv[i], "--backend") && i + 1 < argc)
            cfg.backend = argv[++i];
        else if (!strcmp(argv[i], "--config") && i + 1 < argc)
            i++;
        else if (!strcmp(argv[i], "--save-config") && i + 1 < argc)
            i++;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))
        {
            usage(argv[0]);
            cfg_free(conf);
            return 0;
        }
        else if (argv[i][0] == '-')
        {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            usage(argv[0]);
            cfg_free(conf);
            return 2;
        }
        else if (nfiles < 64)
            files[nfiles++] = argv[i];
    }
    if (cfg.secs > 0) cfg.ops = 0;

    const char *const *wl = files;
    int nwl = nfiles;
    if (nwl == 0 && ncfiles > 0)
    {
        wl = cfiles;
        nwl = ncfiles;
    }
    if (nwl == 0)
    {
        usage(argv[0]);
        cfg_free(conf);
        return 2;
    }

    const char *engines[16];
    int neng = 0;
    if (cfg.backend)
    {
        char buf[256];
        snprintf(buf, sizeof buf, "%s", cfg.backend);
        for (char *t = strtok(buf, ","); t && neng < 16; t = strtok(NULL, ","))
        {
            const backend_entry *e = backend_find(t);
            if (!e)
            {
                fprintf(stderr, "unknown backend '%s' (have: ", t);
                list_backends();
                fprintf(stderr, ")\n");
                cfg_free(conf);
                return 2;
            }
            engines[neng++] = e->name;
        }
    }
    if (neng == 0) engines[neng++] = backend_find(NULL)->name;

    int tlist[64] = {1}, blist[64] = {1}, ntp = 1, nbp = 1;
    if (threads_arg)
    {
        ntp = parse_int_list(threads_arg, tlist, 64);
        if (ntp < 1)
        {
            tlist[0] = 1;
            ntp = 1;
        }
    }
    if (batch_arg)
    {
        nbp = parse_int_list(batch_arg, blist, 64);
        if (nbp < 1)
        {
            blist[0] = 1;
            nbp = 1;
        }
    }
    if (repeat < 1) repeat = 1;

    char rd_spec[2400], rd_replay[700];
    if (!report_dir_arg && conf)
        report_dir_arg = kv_opt_str(cfg_section(conf, "bench"), "report_dir", NULL);
    if (report_dir_arg)
    {
        if (setup_report_dir(report_dir_arg, rd_spec, sizeof rd_spec, rd_replay,
                             sizeof rd_replay) != 0)
        {
            cfg_free(conf);
            return 2;
        }
        report_arg = rd_spec; /* bundle console + tsv + timeline into the dir */
        save_arg = rd_replay; /* and drop the replay config alongside them */
    }

    if (save_arg)
        save_config(save_arg, &cfg, cfg.backend ? cfg.backend : engines[0],
                    threads_arg ? threads_arg : "1", batch_arg ? batch_arg : "1", repeat,
                    report_arg, report_dir_arg, wl, nwl, conf);

    if (!probe_arg && conf)
    {
        const kv_options *b = cfg_section(conf, "bench");
        probe_arg = kv_opt_str(b, "probe", NULL);
    }
    if (!probe_arg)
    {
        probe_select_all();
    }
    else if (!strcmp(probe_arg, "none"))
    {
        probe_select_none();
    }
    else
    {
        char buf[256];
        snprintf(buf, sizeof buf, "%s", probe_arg);
        for (char *t = strtok(buf, ","); t; t = strtok(NULL, ","))
            if (probe_select(t) != 0) fprintf(stderr, "unknown probe '%s' (skipped)\n", t);
    }

    reporter_set reporters;
    if (reporters_parse(report_arg, &reporters) != 0)
    {
        cfg_free(conf);
        return 2;
    }

    report_run run = {
        .engine = cfg.backend ? cfg.backend : engines[0],
        .seed = cfg.seed,
        .repeat = repeat,
        .threads_list = threads_arg ? threads_arg : "1",
        .batch_list = batch_arg ? batch_arg : "1",
        .nworkloads = nwl,
    };
    reporters_run(&reporters, &run);
    probe_set_data_dir(cfg.data_dir);
    emit_probes(&reporters);

    for (int i = 0; i < nwl; i++)
        run_file(wl[i], &cfg, engines, neng, tlist, ntp, blist, nbp, repeat, &reporters);

    reporters_close(&reporters);
    cfg_free(conf);
    return 0;
}
