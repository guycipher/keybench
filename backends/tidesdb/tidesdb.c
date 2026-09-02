#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <tidesdb/db.h>
#include <tidesdb/tidesdb_version.h>
#include <time.h>
#include <unistd.h>

#include "backend_registry.h"
#include "bench.h"

#define TDB_CF_NAME  "keybench"
#define TDB_NO_TTL   ((time_t)0)
#define TDB_NFTW_FDS 16

/* tidesdb 10 reports at most this many levels per column family */
#define TDB_REPORT_LEVELS  TDB_MAX_LEVELS
#define TDB_LEVEL_NAME_LEN 24

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

static tidesdb_compression_algorithm_t comp_from_name(const char *s,
                                                      tidesdb_compression_algorithm_t dflt)
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

/* tidesdb 10 dropped the debug and fatal levels; the nearest surviving level is accepted for each
   so an existing workload file keeps working */
static tidesdb_log_level_t log_from_name(const char *s, tidesdb_log_level_t dflt)
{
    if (!s) return dflt;
    if (!strcmp(s, "none")) return TDB_LOG_NONE;
    if (!strcmp(s, "trace") || !strcmp(s, "debug")) return TDB_LOG_TRACE;
    if (!strcmp(s, "info")) return TDB_LOG_INFO;
    if (!strcmp(s, "warn")) return TDB_LOG_WARN;
    if (!strcmp(s, "error") || !strcmp(s, "fatal")) return TDB_LOG_ERROR;
    return dflt;
}

/* the memtable, its WAL sync, and the L0 stall threshold are database level in tidesdb 10, having
   been per column family in 9. both option spellings are accepted -- the v10 name and the v9 one
   the existing workload files use -- so a workload does not have to be rewritten to move a knob */
static void tdb_apply_db_config(tidesdb_config_t *c, const kv_options *o)
{
    c->num_flush_threads = (int)kv_opt_int(o, "num_flush_threads", c->num_flush_threads);
    c->num_compaction_threads =
        (int)kv_opt_int(o, "num_compaction_threads", c->num_compaction_threads);
    c->block_cache_size = (size_t)kv_opt_int(o, "block_cache_size", (long long)c->block_cache_size);
    c->max_open_sstables =
        (size_t)kv_opt_int(o, "max_open_sstables", (long long)c->max_open_sstables);
    c->log_level = log_from_name(kv_opt_str(o, "log_level", NULL), c->log_level);

    const long long buffer =
        kv_opt_int(o, "write_buffer_size", (long long)c->memtable_write_buffer_size);
    c->memtable_write_buffer_size = (size_t)kv_opt_int(o, "memtable_write_buffer_size", buffer);

    const int max_level =
        (int)kv_opt_int(o, "skip_list_max_level", c->memtable_skip_list_max_level);
    c->memtable_skip_list_max_level = (int)kv_opt_int(o, "memtable_skip_list_max_level", max_level);

    const char *s;
    if ((s = kv_opt_str(o, "skip_list_probability", NULL)))
        c->memtable_skip_list_probability = (float)atof(s);
    if ((s = kv_opt_str(o, "memtable_skip_list_probability", NULL)))
        c->memtable_skip_list_probability = (float)atof(s);

    c->memtable_sync_mode = sync_from_name(kv_opt_str(o, "sync_mode", NULL), c->memtable_sync_mode);
    const long long interval =
        kv_opt_int(o, "sync_interval_us", (long long)c->memtable_sync_interval_us);
    c->memtable_sync_interval_us = (uint64_t)kv_opt_int(o, "memtable_sync_interval_us", interval);

    const int stall =
        (int)kv_opt_int(o, "l0_queue_stall_threshold", c->memtable_l0_queue_stall_threshold);
    c->memtable_l0_queue_stall_threshold =
        (int)kv_opt_int(o, "memtable_l0_queue_stall_threshold", stall);

    /* tidesdb 10 knobs with no tidesdb 9 spelling, so each takes its v10 name only. The value log
       segment size changes what a reclaim costs rather than where the store settles, the idle flush
       is a write a quiet process would not otherwise make, and the transaction timeout bounds an
       abandoned transaction -- all three are worth being able to move under a benchmark. */
    c->vlog_segment_size =
        (size_t)kv_opt_int(o, "vlog_segment_size", (long long)c->vlog_segment_size);
    c->memtable_idle_flush_seconds =
        (int)kv_opt_int(o, "memtable_idle_flush_seconds", c->memtable_idle_flush_seconds);
    c->txn_timeout_seconds =
        (int64_t)kv_opt_int(o, "txn_timeout_seconds", (long long)c->txn_timeout_seconds);

    /* the log sink is process wide rather than per database, so a run that turns logging on writes
       one tidesdb.log inside the db directory it last opened */
    c->log_to_file = (int)kv_opt_int(o, "log_to_file", c->log_to_file);
    c->log_truncation_at =
        (size_t)kv_opt_int(o, "log_truncation_at", (long long)c->log_truncation_at);

    /* the value separation cut-off became database level, the value log being one shared structure,
       having been the per column family klog_value_threshold. the old spelling is still accepted so
       an existing workload file keeps working, and the column family now decides only whether to
       opt out of separation entirely, through keep_values_inline. */
    const long long sep =
        kv_opt_int(o, "klog_value_threshold", (long long)c->value_separation_threshold);
    c->value_separation_threshold = (size_t)kv_opt_int(o, "value_separation_threshold", sep);
}

/* tidesdb 10 replaced the single compression_algorithm field with a stackable encoding pipeline, so
   a compression choice becomes a one-stage pipeline and "none" an empty one */
static void tdb_apply_encoding(tidesdb_column_family_config_t *cf, const kv_options *o)
{
    const char *s = kv_opt_str(o, "compression", NULL);
    if (!s) return;

    const tidesdb_compression_algorithm_t algo = comp_from_name(s, TDB_COMPRESS_NONE);
    if (algo == TDB_COMPRESS_NONE)
    {
        cf->encoding_count = 0;
        return;
    }
    cf->encoding_pipeline[0] = (uint8_t)algo;
    cf->encoding_count = 1;
}

static void tdb_apply_cf_config(tidesdb_column_family_config_t *cf, const kv_options *o)
{
    const char *s;
    cf->level_size_ratio =
        (size_t)kv_opt_int(o, "level_size_ratio", (long long)cf->level_size_ratio);
    cf->min_levels = (int)kv_opt_int(o, "min_levels", cf->min_levels);
    cf->dividing_level_offset =
        (int)kv_opt_int(o, "dividing_level_offset", cf->dividing_level_offset);
    /* 1 keeps every value in the key log whatever its size, ignoring the database's separation
       threshold. That is the storage strategy rocksdb uses with BlobDB off, so it is the knob that
       makes a like for like comparison possible from this side: separation is what keeps the tree
       small and compaction cheap, and turning it off is what shows the cost it was paying for. */
    cf->keep_values_inline = (int)kv_opt_int(o, "keep_values_inline", cf->keep_values_inline);
    cf->btree_klog_block_size =
        (size_t)kv_opt_int(o, "btree_klog_block_size", (long long)cf->btree_klog_block_size);
    tdb_apply_encoding(cf, o);
    cf->enable_bloom_filter = (int)kv_opt_int(o, "enable_bloom_filter", cf->enable_bloom_filter);
    if ((s = kv_opt_str(o, "bloom_fpr", NULL))) cf->bloom_fpr = atof(s);
    cf->default_isolation_level =
        iso_from_name(kv_opt_str(o, "isolation", NULL), cf->default_isolation_level);
    cf->l1_file_count_trigger =
        (int)kv_opt_int(o, "l1_file_count_trigger", cf->l1_file_count_trigger);
    if ((s = kv_opt_str(o, "tombstone_density_trigger", NULL)))
        cf->tombstone_density_trigger = atof(s);
    cf->tombstone_density_min_entries = (uint64_t)kv_opt_int(
        o, "tombstone_density_min_entries", (long long)cf->tombstone_density_min_entries);
}

/* TDB_ERR_LOCKED is transient lock contention under concurrent writers and TDB_ERR_CONFLICT is a
   lost first-committer-wins race, which only the snapshot and serializable levels produce. both are
   worth waiting out, so the write ops surface them as KV_RETRY and the store retries rather than
   dropping the write -- a plain rocksdb put has no conflict detection to lose, so retrying here is
   what keeps the two engines doing the same amount of work. every other non success code is a real
   failure. */
static int tdb_retryable(int rc)
{
    return rc == TDB_ERR_LOCKED || rc == TDB_ERR_CONFLICT;
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

/* the caller frees the returned value with libc free, but the engine allocated it with its own
   allocator (a jemalloc build routes through jemalloc), so hand back a copy and release the
   engine's buffer through tidesdb_free -- the same handover the rocksdb backend does */
static int tdb_get(void *ctx, const char *k, size_t klen, const char **vp, size_t *vlen)
{
    tdb *t = ctx;
    tidesdb_txn_t *txn = NULL;
    if (tidesdb_txn_begin(t->db, &txn) != TDB_SUCCESS) return KV_ERR;
    uint8_t *val = NULL;
    size_t vs = 0;
    const int rc = tidesdb_txn_get(txn, t->cf, (const uint8_t *)k, klen, &val, &vs);
    tidesdb_txn_free(txn);

    if (rc == TDB_SUCCESS && val)
    {
        char *copy = xmalloc(vs);
        memcpy(copy, val, vs);
        tidesdb_free(val);
        *vp = copy;
        *vlen = vs;
        return 1;
    }
    if (rc == TDB_ERR_NOT_FOUND) return 0;
    return KV_ERR;
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

        /* tidesdb_iter_key_value allocates a fresh key and value on every step and hands
           both to the caller to free, unlike the rocksdb iterator, whose key and value
           borrow the iterator's own buffer and must not be freed. The two look alike at
           the call site, so it is worth being explicit: every row has to be released
           before the next step, on the early exits as much as on the common path, or a
           scan leaks its entire result. Leaving this out cost about 411 KB per hundred row
           range, which is roughly 430 MB a second under the mixed workload -- enough to
           take the process to 32 GB and have systemd-oomd kill the run. */
        const int past_hi = kv_keycmp((const char *)k, ks, hi, hilen) >= 0;
        int cb_stop = 0;
        if (!past_hi)
        {
            n++;
            cb_stop = cb && cb(cbarg, (const char *)k, ks, (const char *)v, vs);
        }
        tidesdb_free(k);
        tidesdb_free(v);

        if (past_hi || cb_stop) break;
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

/* The sample callback in main.c stores each metric name by pointer and reads it
   after the stats call returns, so every name must outlive this function. Scalar
   names are string literals. Per-level names need formatting, so they live in a
   static table, which is safe because a single sampler thread runs the stats at
   a time. */
static void tdb_cf_level_stats(const tidesdb_cf_stats_t *st, kv_stat_cb cb, void *arg)
{
    static char names[TDB_REPORT_LEVELS][4][TDB_LEVEL_NAME_LEN];
    int n = st->num_levels;
    if (n > TDB_REPORT_LEVELS) n = TDB_REPORT_LEVELS;
    for (int i = 0; i < n; i++)
    {
        snprintf(names[i][0], TDB_LEVEL_NAME_LEN, "level%d_sstables", i + 1);
        cb(arg, names[i][0], (double)st->level_num_sstables[i]);
        snprintf(names[i][1], TDB_LEVEL_NAME_LEN, "level%d_keys", i + 1);
        cb(arg, names[i][1], (double)st->level_key_counts[i]);
        snprintf(names[i][2], TDB_LEVEL_NAME_LEN, "level%d_bytes", i + 1);
        cb(arg, names[i][2], (double)st->level_sizes[i]);
        snprintf(names[i][3], TDB_LEVEL_NAME_LEN, "level%d_tombstones", i + 1);
        cb(arg, names[i][3], (double)st->level_tombstone_counts[i]);
    }
}

/* tidesdb 10 returns column family stats by value into a caller-owned struct, so there is nothing
   to free, and the memtable figures moved to the database level where the memtable is now shared */
static void tdb_cf_stats(tdb *t, kv_stat_cb cb, void *arg)
{
    tidesdb_cf_stats_t st;
    if (tidesdb_get_cf_stats(t->cf, &st) != TDB_SUCCESS) return;
    cb(arg, "keys", (double)st.total_keys);
    cb(arg, "levels", (double)st.num_levels);
    cb(arg, "data_bytes", (double)st.total_data_size);
    cb(arg, "avg_key_size", st.avg_key_size);
    cb(arg, "avg_value_size", st.avg_value_size);
    cb(arg, "read_amp", st.read_amp);
    cb(arg, "tombstones", (double)st.total_tombstones);
    cb(arg, "tombstone_ratio", st.tombstone_ratio);
    cb(arg, "max_sst_density", st.max_sst_density);
    cb(arg, "max_sst_density_level", (double)st.max_sst_density_level);
    cb(arg, "btree_nodes", (double)st.btree_total_nodes);
    cb(arg, "btree_max_height", (double)st.btree_max_height);
    cb(arg, "btree_avg_height", st.btree_avg_height);
    cb(arg, "compaction_count", (double)st.compaction_count);
    cb(arg, "wal_bytes", (double)st.wal_bytes_written);
    cb(arg, "flush_bytes", (double)st.flush_bytes_written);
    cb(arg, "compaction_bytes", (double)st.compaction_bytes_written);
    cb(arg, "compaction_bytes_read", (double)st.compaction_bytes_read);
    cb(arg, "user_bytes", (double)st.user_bytes_written);
    cb(arg, "unflushed_keys", (double)st.unflushed_key_count);
    /* memory the family's partition range filters hold outside the block cache, one routing
       directory per sstable. block_cache_size does not bound it, and it is built lazily on a
       table's first probe, so it reads zero on a family nothing has read from and climbs as tables
       are touched -- which makes it the term that explains resident memory a cache figure cannot.
     */
    cb(arg, "filter_resident_bytes", (double)st.filter_resident_bytes);
    long sst = 0;
    int n = st.num_levels;
    if (n > TDB_REPORT_LEVELS) n = TDB_REPORT_LEVELS;
    for (int i = 0; i < n; i++) sst += st.level_num_sstables[i];
    cb(arg, "sstables", (double)sst);
    tdb_cf_level_stats(&st, cb, arg);
}

static void tdb_db_core_stats(const tidesdb_db_stats_t *d, kv_stat_cb cb, void *arg)
{
    cb(arg, "total_sstables", (double)d->total_sstable_count);
    cb(arg, "open_sstables", (double)d->num_open_sstables);
    cb(arg, "db_data_bytes", (double)d->total_data_size_bytes);
    cb(arg, "db_memtable_bytes", (double)d->memtable_bytes);
    cb(arg, "immutable_memtables", (double)d->immutable_memtable_count);
    cb(arg, "compaction_queue", (double)d->compaction_pending_count);
    cb(arg, "db_is_flushing", (double)d->is_flushing);
    cb(arg, "txn_memory_bytes", (double)d->txn_memory_bytes);
    cb(arg, "active_txns", (double)d->active_txn_count);
    cb(arg, "db_column_families", (double)d->num_column_families);
    cb(arg, "db_global_seq", (double)d->global_seq);
    cb(arg, "db_min_snapshot_seq", (double)d->min_snapshot_seq);
    cb(arg, "db_wal_generation", (double)d->wal_generation);
}

static void tdb_db_io_stats(const tidesdb_db_stats_t *d, kv_stat_cb cb, void *arg)
{
    cb(arg, "db_flush_count", (double)d->flush_count);
    cb(arg, "db_compaction_count", (double)d->compaction_count);
    cb(arg, "db_flush_bytes", (double)d->flush_bytes_written);
    cb(arg, "db_compaction_bytes", (double)d->compaction_bytes_written);
    cb(arg, "db_compaction_bytes_read", (double)d->compaction_bytes_read);
    cb(arg, "db_user_bytes", (double)d->user_bytes_written);
    cb(arg, "db_vlog_bytes", (double)d->vlog_file_size);
    cb(arg, "db_vlog_values", (double)d->vlog_value_count);
    cb(arg, "db_vlog_used_bytes", (double)d->vlog_used_bytes);

    /* value log occupancy and reclamation. vlog_file_size on its own says only how much disk the
       log sits on, which under a workload that overwrites is mostly superseded versions. live is
       what the sstables can still reach and is the figure space amplification is measured against,
       dead is what a reclaim could recover, and drainable is how much of that is actionable now --
       a drainable figure that stays high is reclamation falling behind. The two reclaim counters
       separate a reclaim that never ran from one that ran and found nothing worth draining. used
       over stored is the encoding pipeline's realised ratio on the data actually held. */
    /* the two terms a write amplification figure needs. The write-ahead log is reclaimed with its
       memtable and never shows up in the on-disk totals, but the device was still asked to write
       it, so a figure that omits it understates by a whole copy of the data. vlog_bytes_written
       counts every value-log append including what reclamation itself rewrites, which on a store
       that separates its values is where most of the writing happens. */
    cb(arg, "db_wal_bytes_written", (double)d->wal_bytes_written);
    cb(arg, "db_vlog_bytes_written", (double)d->vlog_bytes_written);

    cb(arg, "db_vlog_stored_bytes", (double)d->vlog_stored_bytes);
    cb(arg, "db_vlog_live_bytes", (double)d->vlog_live_bytes);
    cb(arg, "db_vlog_dead_bytes", (double)d->vlog_dead_bytes);
    cb(arg, "db_vlog_segments", (double)d->vlog_segment_count);
    cb(arg, "db_vlog_segments_drainable", (double)d->vlog_segments_drainable);
    cb(arg, "db_vlog_segments_retired", (double)d->vlog_segments_retired);
    cb(arg, "db_vlog_reclaim_calls", (double)d->vlog_reclaim_calls);
    cb(arg, "db_vlog_reclaim_passes", (double)d->vlog_reclaim_passes);

    /* write admission -- non-zero means ingestion outran the flush and writers were paced */
    cb(arg, "db_writes_throttled", (double)d->writes_throttled);
    cb(arg, "db_writes_blocked", (double)d->writes_blocked);
    cb(arg, "db_write_stall_us", (double)d->write_stall_us);
    cb(arg, "db_write_stall_ceiling", (double)d->write_stall_ceiling_hits);
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
