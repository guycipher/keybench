#define _POSIX_C_SOURCE 200809L
#include "reporter.h"

#include <stdlib.h>
#include <string.h>

#ifndef KEYBENCH_VERSION
#define KEYBENCH_VERSION "dev"
#endif

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

static void console_run(reporter *r, const report_run *run)
{
    fprintf(r->out, "keybench %s: %d workload(s), engine=%s seed=%u repeat=%d\n", KEYBENCH_VERSION,
            run->nworkloads, run->engine, run->seed, run->repeat);
    fprintf(r->out, "  threads: %s\n", run->threads_list);
}

static void console_probe(reporter *r, const report_probe *p)
{
    fprintf(r->out, "[probe: %s]\n", p->probe);
    for (int i = 0; i < p->n; i++) fprintf(r->out, "  %-14s %s\n", p->kv[i].key, p->kv[i].value);
}

static void console_point(reporter *r, const report_point *p)
{
    if (p->sweep_param)
        fprintf(r->out, "\n=== %s ===\nengine=%s  threads=%d  %s=%ld  (median of %d run%s)\n",
                p->workload, p->engine, p->threads, p->sweep_param, p->sweep_value, p->repeat,
                p->repeat == 1 ? "" : "s");
    else
        fprintf(r->out, "\n=== %s ===\nengine=%s  threads=%d  (median of %d run%s)\n", p->workload,
                p->engine, p->threads, p->repeat, p->repeat == 1 ? "" : "s");
    fprintf(r->out, "%-7s %12s %10s %10s %10s %10s\n", "op", "count", "p50", "p99", "p99.9", "max");
    char b50[32], b99[32], b999[32], bmax[32];
    for (int i = 0; i < p->n_ops; i++)
    {
        const report_op *o = &p->ops[i];
        fmt_ns(o->p50, b50, sizeof b50);
        fmt_ns(o->p99, b99, sizeof b99);
        fmt_ns(o->p999, b999, sizeof b999);
        fmt_ns(o->max, bmax, sizeof bmax);
        fprintf(r->out, "%-7s %12llu %10s %10s %10s %10s\n", o->op, (unsigned long long)o->count,
                b50, b99, b999, bmax);
    }
    if (p->has_hit)
        fprintf(r->out, "get hit rate: %.1f%%%s\n", p->hit_rate, p->repeat > 1 ? " (median)" : "");
    if (p->ops_med == p->wu_med)
    {
        /* Each workload unit is exactly one primitive op (no checkout fan-out or
         * batching), so the two rates coincide and one line says it. */
        fprintf(r->out, "throughput: %.0f /sec (1 op per unit)", p->wu_med);
        if (p->repeat > 1)
            fprintf(r->out, "  (%.0f..%.0f over %d runs)", p->wu_lo, p->wu_hi, p->repeat);
        fprintf(r->out, "\n");
    }
    else
    {
        fprintf(r->out, "workload units: %.0f wu/sec", p->wu_med);
        if (p->repeat > 1)
            fprintf(r->out, "  (%.0f..%.0f over %d runs)", p->wu_lo, p->wu_hi, p->repeat);
        fprintf(r->out, "\nprimitive ops:  %.0f ops/sec", p->ops_med);
        if (p->repeat > 1)
            fprintf(r->out, "  (%.0f..%.0f over %d runs)", p->ops_lo, p->ops_hi, p->repeat);
        fprintf(r->out, "\n");
    }
}

typedef struct
{
    int header_done;
} tsv_ctx;

static void tsv_point(reporter *r, const report_point *p)
{
    tsv_ctx *t = r->ctx;
    if (!t->header_done)
    {
        fprintf(r->out,
                "workload\tengine\tthreads\tsweep_param\tsweep_value\top\tcount"
                "\tp50_ns\tp99_ns\tp999_ns\tmax_ns\thit_rate\twu_per_s\tops_per_s\n");
        t->header_done = 1;
    }
    for (int i = 0; i < p->n_ops; i++)
    {
        const report_op *o = &p->ops[i];
        fprintf(r->out, "%s\t%s\t%d\t%s\t%ld\t%s\t%llu\t%llu\t%llu\t%llu\t%llu\t%.3f\t%.1f\t%.1f\n",
                p->workload, p->engine, p->threads, p->sweep_param ? p->sweep_param : "",
                p->sweep_param ? p->sweep_value : 1, o->op, (unsigned long long)o->count,
                (unsigned long long)o->p50, (unsigned long long)o->p99, (unsigned long long)o->p999,
                (unsigned long long)o->max, p->has_hit ? p->hit_rate : 0.0, p->wu_med, p->ops_med);
    }
    fflush(r->out);
}

static void timeline_sample(reporter *r, const report_sample *s)
{
    tsv_ctx *t = r->ctx;
    if (!t->header_done)
    {
        fprintf(r->out,
                "workload\tengine\tthreads\tsweep_param\tsweep_value\telapsed_s\tmetric\tvalue\n");
        t->header_done = 1;
    }
    for (int i = 0; i < s->nm; i++)
        fprintf(r->out, "%s\t%s\t%d\t%s\t%ld\t%.3f\t%s\t%.3f\n", s->workload, s->engine, s->threads,
                s->sweep_param ? s->sweep_param : "", s->sweep_param ? s->sweep_value : 1,
                s->elapsed_s, s->m[i].name, s->m[i].value);
    fflush(r->out);
}

static reporter *make_reporter(const char *name, FILE *out)
{
    reporter *r = calloc(1, sizeof *r);
    r->out = out;
    if (!strcmp(name, "console"))
    {
        r->name = "console";
        r->on_run = console_run;
        r->on_probe = console_probe;
        r->on_point = console_point;
    }
    else if (!strcmp(name, "tsv"))
    {
        r->name = "tsv";
        r->ctx = calloc(1, sizeof(tsv_ctx));
        r->on_point = tsv_point;
    }
    else if (!strcmp(name, "timeline"))
    {
        r->name = "timeline";
        r->ctx = calloc(1, sizeof(tsv_ctx));
        r->on_sample = timeline_sample;
    }
    else
    {
        free(r);
        return NULL;
    }
    return r;
}

int reporters_parse(const char *spec, reporter_set *set)
{
    set->n = 0;
    char *buf = strdup(spec ? spec : "console");
    if (!buf)
    {
        fprintf(stderr, "keybench: out of memory\n");
        return -1;
    }
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ","))
    {
        if (set->n >= REPORT_MAX_SINKS)
        {
            fprintf(stderr, "too many reporters\n");
            free(buf);
            return -1;
        }
        char *path = strchr(tok, ':');
        if (path) *path++ = '\0';
        FILE *out = stdout;
        if (path && strcmp(path, "-") != 0)
        {
            out = fopen(path, "w");
            if (!out)
            {
                fprintf(stderr, "cannot open report file '%s'\n", path);
                free(buf);
                return -1;
            }
        }
        reporter *r = make_reporter(tok, out);
        if (!r)
        {
            fprintf(stderr, "unknown reporter '%s' (have: console, tsv, timeline)\n", tok);
            if (out != stdout) fclose(out);
            free(buf);
            return -1;
        }
        set->r[set->n++] = r;
    }
    free(buf);
    if (set->n == 0)
    {
        reporter *r = make_reporter("console", stdout);
        set->r[set->n++] = r;
    }
    return 0;
}

void reporters_run(reporter_set *set, const report_run *r)
{
    for (int i = 0; i < set->n; i++)
        if (set->r[i]->on_run) set->r[i]->on_run(set->r[i], r);
}
void reporters_point(reporter_set *set, const report_point *p)
{
    for (int i = 0; i < set->n; i++)
        if (set->r[i]->on_point) set->r[i]->on_point(set->r[i], p);
}
void reporters_probe(reporter_set *set, const report_probe *p)
{
    for (int i = 0; i < set->n; i++)
        if (set->r[i]->on_probe) set->r[i]->on_probe(set->r[i], p);
}
void reporters_sample(reporter_set *set, const report_sample *s)
{
    for (int i = 0; i < set->n; i++)
        if (set->r[i]->on_sample) set->r[i]->on_sample(set->r[i], s);
}
int reporters_want_samples(const reporter_set *set)
{
    for (int i = 0; i < set->n; i++)
        if (set->r[i]->on_sample) return 1;
    return 0;
}
int reporters_want_probe(const reporter_set *set)
{
    for (int i = 0; i < set->n; i++)
        if (set->r[i]->on_probe) return 1;
    return 0;
}
void reporters_close(reporter_set *set)
{
    for (int i = 0; i < set->n; i++)
    {
        reporter *r = set->r[i];
        if (r->on_close) r->on_close(r);
        if (r->out && r->out != stdout && r->out != stderr) fclose(r->out);
        free(r->ctx);
        free(r);
    }
    set->n = 0;
}
