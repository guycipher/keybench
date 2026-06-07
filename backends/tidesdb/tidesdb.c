#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <tidesdb/tidesdb.h>
#include <tidesdb/tidesdb_version.h>
#include <time.h>
#include <unistd.h>

#include "backend_registry.h"
#include "bench.h"

#define TDB_CF_NAME  "keybench"
#define TDB_NO_TTL   ((time_t)0)
#define TDB_NFTW_FDS 16

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
    {
        fprintf(stderr, "keybench: out of memory\n");
        abort();
    }
    return p;
}

typedef struct
{
    tidesdb_t *db;
    tidesdb_column_family_t *cf;
    char path[256];
} tdb;

static compression_algorithm comp_from_name(const char *s, compression_algorithm dflt)
{
    if (!s) return dflt;
    if (!strcmp(s, "none")) return TDB_COMPRESS_NONE;
    if (!strcmp(s, "snappy")) return TDB_COMPRESS_SNAPPY;
    if (!strcmp(s, "lz4")) return TDB_COMPRESS_LZ4;
    if (!strcmp(s, "zstd")) return TDB_COMPRESS_ZSTD;
    if (!strcmp(s, "lz4_fast")) return TDB_COMPRESS_LZ4_FAST;
    return dflt;
}

static int sync_from_name(const char *s, int dflt)
{
    if (!s) return dflt;
    if (!strcmp(s, "none")) return TDB_SYNC_NONE;
    if (!strcmp(s, "full")) return TDB_SYNC_FULL;
    if (!strcmp(s, "interval")) return TDB_SYNC_INTERVAL;
    return dflt;
}

static tidesdb_isolation_level_t iso_from_name(const char *s, tidesdb_isolation_level_t dflt)
{
    if (!s) return dflt;
    if (!strcmp(s, "read_uncommitted")) return TDB_ISOLATION_READ_UNCOMMITTED;
    if (!strcmp(s, "read_committed")) return TDB_ISOLATION_READ_COMMITTED;
    if (!strcmp(s, "repeatable_read")) return TDB_ISOLATION_REPEATABLE_READ;
    if (!strcmp(s, "snapshot")) return TDB_ISOLATION_SNAPSHOT;
    if (!strcmp(s, "serializable")) return TDB_ISOLATION_SERIALIZABLE;
    return dflt;
}

static tidesdb_log_level_t log_from_name(const char *s, tidesdb_log_level_t dflt)
{
    if (!s) return dflt;
    if (!strcmp(s, "none")) return TDB_LOG_NONE;
    if (!strcmp(s, "debug")) return TDB_LOG_DEBUG;
    if (!strcmp(s, "info")) return TDB_LOG_INFO;
    if (!strcmp(s, "warn")) return TDB_LOG_WARN;
    if (!strcmp(s, "error")) return TDB_LOG_ERROR;
    if (!strcmp(s, "fatal")) return TDB_LOG_FATAL;
    return dflt;
}

static void tdb_apply_db_config(tidesdb_config_t *c, const kv_options *o)
{
    c->num_flush_threads = (int)kv_opt_int(o, "num_flush_threads", c->num_flush_threads);
    c->num_compaction_threads =
        (int)kv_opt_int(o, "num_compaction_threads", c->num_compaction_threads);
    c->block_cache_size = (size_t)kv_opt_int(o, "block_cache_size", (long long)c->block_cache_size);
    c->max_open_sstables =
        (size_t)kv_opt_int(o, "max_open_sstables", (long long)c->max_open_sstables);
    c->max_memory_usage = (size_t)kv_opt_int(o, "max_memory_usage", (long long)c->max_memory_usage);
    c->max_concurrent_flushes =
        (int)kv_opt_int(o, "max_concurrent_flushes", c->max_concurrent_flushes);
    c->unified_memtable = (int)kv_opt_int(o, "unified_memtable", c->unified_memtable);
    c->unified_memtable_write_buffer_size = (size_t)kv_opt_int(
        o, "unified_memtable_write_buffer_size", (long long)c->unified_memtable_write_buffer_size);
    c->log_level = log_from_name(kv_opt_str(o, "log_level", NULL), c->log_level);
}

static void tdb_apply_cf_config(tidesdb_column_family_config_t *cf, const kv_options *o)
{
    const char *s;
    cf->write_buffer_size =
        (size_t)kv_opt_int(o, "write_buffer_size", (long long)cf->write_buffer_size);
    cf->level_size_ratio =
        (size_t)kv_opt_int(o, "level_size_ratio", (long long)cf->level_size_ratio);
    cf->min_levels = (int)kv_opt_int(o, "min_levels", cf->min_levels);
    cf->dividing_level_offset =
        (int)kv_opt_int(o, "dividing_level_offset", cf->dividing_level_offset);
    cf->klog_value_threshold =
        (size_t)kv_opt_int(o, "klog_value_threshold", (long long)cf->klog_value_threshold);
    cf->compression_algorithm =
        comp_from_name(kv_opt_str(o, "compression", NULL), cf->compression_algorithm);
    cf->enable_bloom_filter = (int)kv_opt_int(o, "enable_bloom_filter", cf->enable_bloom_filter);
    if ((s = kv_opt_str(o, "bloom_fpr", NULL))) cf->bloom_fpr = atof(s);
    cf->enable_block_indexes = (int)kv_opt_int(o, "enable_block_indexes", cf->enable_block_indexes);
    cf->index_sample_ratio = (int)kv_opt_int(o, "index_sample_ratio", cf->index_sample_ratio);
    cf->block_index_prefix_len =
        (int)kv_opt_int(o, "block_index_prefix_len", cf->block_index_prefix_len);
    cf->sync_mode = sync_from_name(kv_opt_str(o, "sync_mode", NULL), cf->sync_mode);
    cf->sync_interval_us =
        (uint64_t)kv_opt_int(o, "sync_interval_us", (long long)cf->sync_interval_us);
    cf->skip_list_max_level = (int)kv_opt_int(o, "skip_list_max_level", cf->skip_list_max_level);
    if ((s = kv_opt_str(o, "skip_list_probability", NULL)))
        cf->skip_list_probability = (float)atof(s);
    cf->default_isolation_level =
        iso_from_name(kv_opt_str(o, "isolation", NULL), cf->default_isolation_level);
    cf->min_disk_space = (uint64_t)kv_opt_int(o, "min_disk_space", (long long)cf->min_disk_space);
    cf->l1_file_count_trigger =
        (int)kv_opt_int(o, "l1_file_count_trigger", cf->l1_file_count_trigger);
    cf->l0_queue_stall_threshold =
        (int)kv_opt_int(o, "l0_queue_stall_threshold", cf->l0_queue_stall_threshold);
    if ((s = kv_opt_str(o, "tombstone_density_trigger", NULL)))
        cf->tombstone_density_trigger = atof(s);
    cf->tombstone_density_min_entries = (uint64_t)kv_opt_int(
        o, "tombstone_density_min_entries", (long long)cf->tombstone_density_min_entries);
    cf->use_btree = (int)kv_opt_int(o, "use_btree", cf->use_btree);
}

/* TDB_ERR_BUSY is transient backpressure and TDB_ERR_LOCKED is transient lock
   contention under concurrent writers, both worth waiting out. The write ops
   surface them as KV_RETRY so the store waits and retries rather than dropping the
   write. Every other non success code is a real failure. */
static int tdb_retryable(int rc)
{
    return rc == TDB_ERR_BUSY || rc == TDB_ERR_LOCKED;
}

static int tdb_put(void *ctx, const char *k, size_t klen, const char *v, size_t vlen)
{
    tdb *t = ctx;
    tidesdb_txn_t *txn = NULL;
    int rc = tidesdb_txn_begin(t->db, &txn);
    if (rc == TDB_SUCCESS)
    {
        rc = tidesdb_txn_put(txn, t->cf, (const uint8_t *)k, klen, (const uint8_t *)v, vlen,
                             TDB_NO_TTL);
        if (rc == TDB_SUCCESS) rc = tidesdb_txn_commit(txn);
        tidesdb_txn_free(txn);
    }
    if (rc == TDB_SUCCESS) return 0;
    if (tdb_retryable(rc)) return KV_RETRY;
    fprintf(stderr, "tidesdb put failed (%d)\n", rc);
    return -1;
}

static int tdb_putbatch(void *ctx, const char *const *keys, const size_t *klens,
                        const char *const *vals, const size_t *vlens, int n)
{
    tdb *t = ctx;
    tidesdb_txn_t *txn = NULL;
    int rc = tidesdb_txn_begin(t->db, &txn);
    if (rc == TDB_SUCCESS)
    {
        for (int i = 0; i < n && rc == TDB_SUCCESS; i++)
            rc = tidesdb_txn_put(txn, t->cf, (const uint8_t *)keys[i], klens[i],
                                 (const uint8_t *)vals[i], vlens[i], TDB_NO_TTL);
        if (rc == TDB_SUCCESS) rc = tidesdb_txn_commit(txn);
        tidesdb_txn_free(txn);
    }
    if (rc == TDB_SUCCESS) return 0;
    if (tdb_retryable(rc)) return KV_RETRY;
    fprintf(stderr, "tidesdb putbatch failed (%d)\n", rc);
    return -1;
}

static int tdb_delbatch(void *ctx, const char *const *keys, const size_t *klens, int n)
{
    tdb *t = ctx;
    tidesdb_txn_t *txn = NULL;
    int rc = tidesdb_txn_begin(t->db, &txn);
    if (rc == TDB_SUCCESS)
    {
        for (int i = 0; i < n && rc == TDB_SUCCESS; i++)
            rc = tidesdb_txn_delete(txn, t->cf, (const uint8_t *)keys[i], klens[i]);
        if (rc == TDB_SUCCESS) rc = tidesdb_txn_commit(txn);
        tidesdb_txn_free(txn);
    }
    if (rc == TDB_SUCCESS) return 0;
    if (tdb_retryable(rc)) return KV_RETRY;
    fprintf(stderr, "tidesdb delbatch failed (%d)\n", rc);
    return -1;
}

static int tdb_get(void *ctx, const char *k, size_t klen, const char **vp, size_t *vlen)
{
    tdb *t = ctx;
    tidesdb_txn_t *txn = NULL;
    if (tidesdb_txn_begin(t->db, &txn) != TDB_SUCCESS) return 0;
    uint8_t *val = NULL;
    size_t vs = 0;
    int rc = tidesdb_txn_get(txn, t->cf, (const uint8_t *)k, klen, &val, &vs);
    tidesdb_txn_free(txn);
    if (rc == TDB_SUCCESS && val)
    {
        *vp = (const char *)val;
        *vlen = vs;
        return 1;
    }
    return 0;
}

static int tdb_del(void *ctx, const char *k, size_t klen)
{
    tdb *t = ctx;
    tidesdb_txn_t *txn = NULL;
    int rc = tidesdb_txn_begin(t->db, &txn);
    if (rc == TDB_SUCCESS)
    {
        rc = tidesdb_txn_delete(txn, t->cf, (const uint8_t *)k, klen);
        if (rc == TDB_SUCCESS) rc = tidesdb_txn_commit(txn);
        tidesdb_txn_free(txn);
    }
    if (rc == TDB_SUCCESS) return 1;
    if (tdb_retryable(rc)) return KV_RETRY;
    return 0;
}

static int tdb_range(void *ctx, const char *lo, size_t lolen, const char *hi, size_t hilen,
                     int limit, kv_range_cb cb, void *cbarg)
{
    tdb *t = ctx;
    tidesdb_txn_t *txn = NULL;
    if (tidesdb_txn_begin(t->db, &txn) != TDB_SUCCESS) return 0;
    tidesdb_iter_t *it = NULL;
    if (tidesdb_iter_new(txn, t->cf, &it) != TDB_SUCCESS)
    {
        tidesdb_txn_free(txn);
        return 0;
    }
    tidesdb_iter_seek(it, (const uint8_t *)lo, lolen);
    int n = 0;
    while (tidesdb_iter_valid(it) && (limit <= 0 || n < limit))
    {
        uint8_t *k = NULL, *v = NULL;
        size_t ks = 0, vs = 0;
        if (tidesdb_iter_key_value(it, &k, &ks, &v, &vs) != TDB_SUCCESS) break;
        if (kv_keycmp((const char *)k, ks, hi, hilen) >= 0) break;
        n++;
        if (cb && cb(cbarg, (const char *)k, ks, (const char *)v, vs)) break;
        if (tidesdb_iter_next(it) != TDB_SUCCESS) break;
    }
    tidesdb_iter_free(it);
    tidesdb_txn_free(txn);
    return n;
}

static const char *tdb_version(void *ctx)
{
    (void)ctx;
    return "tidesdb " TIDESDB_VERSION;
}

static const char *tdb_datadir(void *ctx)
{
    return ((tdb *)ctx)->path;
}

#define TDB_MAX_REPORT_LEVELS 16
#define TDB_LEVEL_NAME_LEN    24

/* The sample callback in main.c stores each metric name by pointer and reads it
   after the stats call returns, so every name must outlive this function. Scalar
   names are string literals. Per-level names need formatting, so they live in a
   static table, which is safe because a single sampler thread runs the stats at
   a time. */
static void tdb_cf_level_stats(const tidesdb_stats_t *st, kv_stat_cb cb, void *arg)
{
    static char names[TDB_MAX_REPORT_LEVELS][4][TDB_LEVEL_NAME_LEN];
    int n = st->num_levels;
    if (n > TDB_MAX_REPORT_LEVELS) n = TDB_MAX_REPORT_LEVELS;
    for (int i = 0; i < n; i++)
    {
        if (st->level_num_sstables)
        {
            snprintf(names[i][0], TDB_LEVEL_NAME_LEN, "level%d_sstables", i);
            cb(arg, names[i][0], (double)st->level_num_sstables[i]);
        }
        if (st->level_key_counts)
        {
            snprintf(names[i][1], TDB_LEVEL_NAME_LEN, "level%d_keys", i);
            cb(arg, names[i][1], (double)st->level_key_counts[i]);
        }
        if (st->level_sizes)
        {
            snprintf(names[i][2], TDB_LEVEL_NAME_LEN, "level%d_bytes", i);
            cb(arg, names[i][2], (double)st->level_sizes[i]);
        }
        if (st->level_tombstone_counts)
        {
            snprintf(names[i][3], TDB_LEVEL_NAME_LEN, "level%d_tombstones", i);
            cb(arg, names[i][3], (double)st->level_tombstone_counts[i]);
        }
    }
}

static void tdb_cf_stats(tdb *t, kv_stat_cb cb, void *arg)
{
    tidesdb_stats_t *st = NULL;
    if (tidesdb_get_stats(t->cf, &st) != TDB_SUCCESS || !st) return;
    cb(arg, "keys", (double)st->total_keys);
    cb(arg, "levels", (double)st->num_levels);
    cb(arg, "memtable_bytes", (double)st->memtable_size);
    cb(arg, "data_bytes", (double)st->total_data_size);
    cb(arg, "avg_key_size", st->avg_key_size);
    cb(arg, "avg_value_size", st->avg_value_size);
    cb(arg, "read_amp", st->read_amp);
    cb(arg, "cf_hit_rate", st->hit_rate);
    cb(arg, "tombstones", (double)st->total_tombstones);
    cb(arg, "tombstone_ratio", st->tombstone_ratio);
    cb(arg, "max_sst_density", st->max_sst_density);
    cb(arg, "max_sst_density_level", (double)st->max_sst_density_level);
    cb(arg, "use_btree", (double)st->use_btree);
    cb(arg, "btree_nodes", (double)st->btree_total_nodes);
    cb(arg, "btree_max_height", (double)st->btree_max_height);
    cb(arg, "btree_avg_height", st->btree_avg_height);
    cb(arg, "flush_count", (double)st->flush_count);
    cb(arg, "compaction_count", (double)st->compaction_count);
    cb(arg, "wal_bytes", (double)st->wal_bytes_written);
    cb(arg, "flush_bytes", (double)st->flush_bytes_written);
    cb(arg, "compaction_bytes", (double)st->compaction_bytes_written);
    cb(arg, "compaction_bytes_read", (double)st->compaction_bytes_read);
    cb(arg, "user_bytes", (double)st->user_bytes_written);
    long sst = 0;
    for (int i = 0; i < st->num_levels; i++) sst += st->level_num_sstables[i];
    cb(arg, "sstables", (double)sst);
    tdb_cf_level_stats(st, cb, arg);
    tidesdb_free_stats(st);
}

static void tdb_db_core_stats(const tidesdb_db_stats_t *d, kv_stat_cb cb, void *arg)
{
    cb(arg, "total_sstables", (double)d->total_sstable_count);
    cb(arg, "open_sstables", (double)d->num_open_sstables);
    cb(arg, "db_data_bytes", (double)d->total_data_size_bytes);
    cb(arg, "db_memtable_bytes", (double)d->total_memtable_bytes);
    cb(arg, "flush_queue", (double)d->flush_queue_size);
    cb(arg, "compaction_queue", (double)d->compaction_queue_size);
    cb(arg, "immutable_memtables", (double)d->total_immutable_count);
    cb(arg, "txn_memory_bytes", (double)d->txn_memory_bytes);
    cb(arg, "db_column_families", (double)d->num_column_families);
    cb(arg, "db_total_memory", (double)d->total_memory);
    cb(arg, "db_available_memory", (double)d->available_memory);
    cb(arg, "db_memory_limit", (double)d->resolved_memory_limit);
    cb(arg, "db_memory_pressure", (double)d->memory_pressure_level);
    cb(arg, "db_flush_pending", (double)d->flush_pending_count);
    cb(arg, "db_global_seq", (double)d->global_seq);
    cb(arg, "db_unified_enabled", (double)d->unified_memtable_enabled);
    cb(arg, "db_unified_memtable_bytes", (double)d->unified_memtable_bytes);
    cb(arg, "db_unified_immutable", (double)d->unified_immutable_count);
    cb(arg, "db_unified_flushing", (double)d->unified_is_flushing);
    cb(arg, "db_unified_wal_gen", (double)d->unified_wal_generation);
}

static void tdb_db_io_stats(const tidesdb_db_stats_t *d, kv_stat_cb cb, void *arg)
{
    cb(arg, "db_objstore_enabled", (double)d->object_store_enabled);
    cb(arg, "db_local_cache_bytes", (double)d->local_cache_bytes_used);
    cb(arg, "db_local_cache_max", (double)d->local_cache_bytes_max);
    cb(arg, "db_local_cache_files", (double)d->local_cache_num_files);
    cb(arg, "db_upload_queue", (double)d->upload_queue_depth);
    cb(arg, "db_uploads", (double)d->total_uploads);
    cb(arg, "db_upload_failures", (double)d->total_upload_failures);
    cb(arg, "db_replica_mode", (double)d->replica_mode);
    cb(arg, "db_uwal_bytes", (double)d->uwal_bytes_written);
    cb(arg, "db_wal_bytes", (double)d->wal_bytes_written);
    cb(arg, "db_flush_bytes", (double)d->flush_bytes_written);
    cb(arg, "db_compaction_bytes", (double)d->compaction_bytes_written);
    cb(arg, "db_compaction_bytes_read", (double)d->compaction_bytes_read);
    cb(arg, "db_user_bytes", (double)d->user_bytes_written);
    cb(arg, "db_flush_count", (double)d->flush_count);
    cb(arg, "db_compaction_count", (double)d->compaction_count);
}

static void tdb_cache_stats(tdb *t, kv_stat_cb cb, void *arg)
{
    tidesdb_cache_stats_t c;
    if (tidesdb_get_cache_stats(t->db, &c) != TDB_SUCCESS || !c.enabled) return;
    cb(arg, "cache_entries", (double)c.total_entries);
    cb(arg, "cache_bytes", (double)c.total_bytes);
    cb(arg, "cache_hits", (double)c.hits);
    cb(arg, "cache_misses", (double)c.misses);
    cb(arg, "cache_hit_rate", c.hit_rate);
    cb(arg, "cache_partitions", (double)c.num_partitions);
}

static void tdb_stats(void *ctx, kv_stat_cb cb, void *arg)
{
    tdb *t = ctx;
    tdb_cf_stats(t, cb, arg);
    tidesdb_db_stats_t d;
    if (tidesdb_get_db_stats(t->db, &d) == TDB_SUCCESS)
    {
        tdb_db_core_stats(&d, cb, arg);
        tdb_db_io_stats(&d, cb, arg);
    }
    tdb_cache_stats(t, cb, arg);
}

static int tdb_rm_entry(const char *p, const struct stat *s, int flag, struct FTW *w)
{
    (void)s;
    (void)flag;
    (void)w;
    remove(p);
    return 0;
}

static void tdb_close(void *ctx)
{
    tdb *t = ctx;
    tidesdb_close(t->db);
    nftw(t->path, tdb_rm_entry, TDB_NFTW_FDS, FTW_DEPTH | FTW_PHYS);
    free(t);
}

kv_backend *tidesdb_backend_open(unsigned seed, const char *data_dir, const kv_options *opts)
{
    (void)seed;
    tdb *t = xmalloc(sizeof *t);

    const char *base = kv_opt_str(opts, "data_dir", data_dir);
    if (!base || !*base) base = "/tmp";
    snprintf(t->path, sizeof t->path, "%s/keybench-tidesdb-XXXXXX", base);
    if (!mkdtemp(t->path))
    {
        perror("mkdtemp");
        abort();
    }

    tidesdb_config_t cfg = tidesdb_default_config();
    cfg.db_path = t->path;
    cfg.log_level = TDB_LOG_NONE;
    tdb_apply_db_config(&cfg, opts);

    t->db = NULL;
    if (tidesdb_open(&cfg, &t->db) != TDB_SUCCESS)
    {
        fprintf(stderr, "tidesdb open failed at %s\n", t->path);
        abort();
    }

    tidesdb_column_family_config_t cf = tidesdb_default_column_family_config();
    snprintf(cf.name, sizeof cf.name, "%s", TDB_CF_NAME);
    tdb_apply_cf_config(&cf, opts);
    if (tidesdb_create_column_family(t->db, TDB_CF_NAME, &cf) != TDB_SUCCESS)
    {
        fprintf(stderr, "tidesdb create_column_family failed\n");
        abort();
    }
    t->cf = tidesdb_get_column_family(t->db, TDB_CF_NAME);
    if (!t->cf)
    {
        fprintf(stderr, "tidesdb get_column_family failed\n");
        abort();
    }

    kv_backend *be = xmalloc(sizeof *be);
    memset(be, 0, sizeof *be);
    be->name = "tidesdb";
    be->ctx = t;
    be->put = tdb_put;
    be->putbatch = tdb_putbatch;
    be->get = tdb_get;
    be->del = tdb_del;
    be->delbatch = tdb_delbatch;
    be->range = tdb_range;
    be->close = tdb_close;
    be->version = tdb_version;
    be->stats = tdb_stats;
    be->datadir = tdb_datadir;
    return be;
}

KV_REGISTER_BACKEND("tidesdb", tidesdb_backend_open, 1);
