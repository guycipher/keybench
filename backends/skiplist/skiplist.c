#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend_registry.h"
#include "bench.h"

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

#define SL_MAXLEVEL 24
#define SL_P        4

typedef struct sl_node
{
    char *key;
    size_t klen;
    char *val;
    size_t vlen;
    int level;
    struct sl_node *next[];
} sl_node;

typedef struct
{
    sl_node *head;
    int level;
    uint64_t rng;
    long count;
    pthread_rwlock_t lock;
} skiplist;

#define kcmp kv_keycmp

static sl_node *node_new(int level, const char *k, size_t klen, const char *v, size_t vlen)
{
    sl_node *n = xmalloc(sizeof(sl_node) + (size_t)level * sizeof(sl_node *));
    n->level = level;
    n->key = xmalloc(klen);
    memcpy(n->key, k, klen);
    n->klen = klen;
    n->val = xmalloc(vlen);
    memcpy(n->val, v, vlen);
    n->vlen = vlen;
    for (int i = 0; i < level; i++) n->next[i] = NULL;
    return n;
}

static int rand_level(skiplist *s)
{
    uint64_t x = s->rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    s->rng = x;
    int lvl = 1;
    while (lvl < SL_MAXLEVEL && (x % SL_P) == 0)
    {
        lvl++;
        x /= SL_P;
    }
    return lvl;
}

static int sl_put(void *ctx, const char *k, size_t klen, const char *v, size_t vlen)
{
    skiplist *s = ctx;
    pthread_rwlock_wrlock(&s->lock);
    sl_node *update[SL_MAXLEVEL];
    sl_node *x = s->head;
    for (int i = s->level - 1; i >= 0; i--)
    {
        while (x->next[i] && kcmp(x->next[i]->key, x->next[i]->klen, k, klen) < 0) x = x->next[i];
        update[i] = x;
    }
    x = x->next[0];
    if (x && kcmp(x->key, x->klen, k, klen) == 0)
    {
        free(x->val);
        x->val = xmalloc(vlen);
        memcpy(x->val, v, vlen);
        x->vlen = vlen;
        pthread_rwlock_unlock(&s->lock);
        return 0;
    }
    int lvl = rand_level(s);
    if (lvl > s->level)
    {
        for (int i = s->level; i < lvl; i++) update[i] = s->head;
        KV_RELAXED_SET(s->level, lvl);
    }
    sl_node *n = node_new(lvl, k, klen, v, vlen);
    for (int i = 0; i < lvl; i++)
    {
        n->next[i] = update[i]->next[i];
        update[i]->next[i] = n;
    }
    KV_RELAXED_ADD(s->count, 1);
    pthread_rwlock_unlock(&s->lock);
    return 0;
}

static int sl_get(void *ctx, const char *k, size_t klen, const char **vp, size_t *vlen)
{
    skiplist *s = ctx;
    pthread_rwlock_rdlock(&s->lock);
    sl_node *x = s->head;
    for (int i = s->level - 1; i >= 0; i--)
        while (x->next[i] && kcmp(x->next[i]->key, x->next[i]->klen, k, klen) < 0) x = x->next[i];
    x = x->next[0];
    int found = 0;
    if (x && kcmp(x->key, x->klen, k, klen) == 0)
    {
        char *c = xmalloc(x->vlen);
        memcpy(c, x->val, x->vlen);
        *vp = c;
        *vlen = x->vlen;
        found = 1;
    }
    pthread_rwlock_unlock(&s->lock);
    return found;
}

static int sl_del(void *ctx, const char *k, size_t klen)
{
    skiplist *s = ctx;
    pthread_rwlock_wrlock(&s->lock);
    sl_node *update[SL_MAXLEVEL];
    sl_node *x = s->head;
    for (int i = s->level - 1; i >= 0; i--)
    {
        while (x->next[i] && kcmp(x->next[i]->key, x->next[i]->klen, k, klen) < 0) x = x->next[i];
        update[i] = x;
    }
    x = x->next[0];
    if (!x || kcmp(x->key, x->klen, k, klen) != 0)
    {
        pthread_rwlock_unlock(&s->lock);
        return 0;
    }
    for (int i = 0; i < s->level; i++)
        if (update[i]->next[i] == x) update[i]->next[i] = x->next[i];
    int lvl = s->level;
    while (lvl > 1 && s->head->next[lvl - 1] == NULL) lvl--;
    KV_RELAXED_SET(s->level, lvl);
    free(x->key);
    free(x->val);
    free(x);
    KV_RELAXED_ADD(s->count, -1);
    pthread_rwlock_unlock(&s->lock);
    return 1;
}

static const char *sl_version(void *ctx)
{
    (void)ctx;
    return "skiplist";
}

static void sl_stats(void *ctx, kv_stat_cb cb, void *arg)
{
    skiplist *s = ctx;
    cb(arg, "keys", (double)KV_RELAXED_GET(s->count));
    cb(arg, "level", (double)KV_RELAXED_GET(s->level));
}

static int sl_range(void *ctx, const char *lo, size_t lolen, const char *hi, size_t hilen,
                    int limit, kv_range_cb cb, void *cbarg)
{
    skiplist *s = ctx;
    pthread_rwlock_rdlock(&s->lock);
    sl_node *x = s->head;

    for (int i = s->level - 1; i >= 0; i--)
        while (x->next[i] && kcmp(x->next[i]->key, x->next[i]->klen, lo, lolen) < 0) x = x->next[i];
    x = x->next[0];
    int n = 0;
    while (x && (limit <= 0 || n < limit))
    {
        if (kcmp(x->key, x->klen, hi, hilen) >= 0) break;
        if (cb && cb(cbarg, x->key, x->klen, x->val, x->vlen)) break;
        n++;
        x = x->next[0];
    }
    pthread_rwlock_unlock(&s->lock);
    return n;
}

static void sl_close(void *ctx)
{
    skiplist *s = ctx;
    sl_node *x = s->head->next[0];
    while (x)
    {
        sl_node *nx = x->next[0];
        free(x->key);
        free(x->val);
        free(x);
        x = nx;
    }
    free(s->head);
    pthread_rwlock_destroy(&s->lock);
    free(s);
}

kv_backend *skiplist_backend_open(unsigned seed, const char *data_dir, const kv_options *opts)
{
    (void)opts;
    (void)data_dir;
    skiplist *s = xmalloc(sizeof(*s));
    memset(s, 0, sizeof(*s));
    pthread_rwlock_init(&s->lock, NULL);
    s->level = 1;

    s->rng = 0x9e3779b97f4a7c15ULL ^ ((uint64_t)seed * 2654435761u + 1u);
    s->head = xmalloc(sizeof(sl_node) + SL_MAXLEVEL * sizeof(sl_node *));
    s->head->level = SL_MAXLEVEL;
    s->head->key = NULL;
    s->head->klen = 0;
    s->head->val = NULL;
    s->head->vlen = 0;
    for (int i = 0; i < SL_MAXLEVEL; i++) s->head->next[i] = NULL;

    kv_backend *be = xmalloc(sizeof(*be));
    memset(be, 0, sizeof(*be));
    be->name = "skiplist";
    be->ctx = s;
    be->put = sl_put;
    be->get = sl_get;
    be->del = sl_del;
    be->range = sl_range;
    be->close = sl_close;
    be->version = sl_version;
    be->stats = sl_stats;
    return be;
}

KV_REGISTER_BACKEND("skiplist", skiplist_backend_open, 0);
