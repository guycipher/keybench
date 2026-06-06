#include "hist.h"

#include <string.h>

void hist_reset(hist *h)
{
    memset(h, 0, sizeof(*h));
    h->min = UINT64_MAX;
}

static int hist_index(uint64_t v)
{
    if (v == 0) return 0;

    int oct = 63 - __builtin_clzll(v);
    if (oct >= HIST_OCT) oct = HIST_OCT - 1;
    uint64_t base = (uint64_t)1 << oct;

    uint64_t slot = ((v - base) * HIST_SUB) / base;
    if (slot >= HIST_SUB) slot = HIST_SUB - 1;
    return oct * HIST_SUB + (int)slot;
}

static uint64_t hist_value_at(int idx)
{
    int oct = idx / HIST_SUB;
    int slot = idx % HIST_SUB;
    uint64_t base = (uint64_t)1 << oct;
    uint64_t width = base / HIST_SUB;
    if (width == 0) width = 1;
    return base + (uint64_t)slot * width + width / 2;
}

void hist_record(hist *h, uint64_t value)
{
    h->buckets[hist_index(value)]++;
    h->count++;
    h->sum += value;
    if (value < h->min) h->min = value;
    if (value > h->max) h->max = value;
}

void hist_merge(hist *dst, const hist *src)
{
    for (int i = 0; i < HIST_BUCKETS; i++) dst->buckets[i] += src->buckets[i];
    dst->count += src->count;
    dst->sum += src->sum;
    if (src->min < dst->min) dst->min = src->min;
    if (src->max > dst->max) dst->max = src->max;
}

uint64_t hist_percentile(const hist *h, double p)
{
    if (h->count == 0) return 0;
    if (p <= 0) return h->min;
    if (p >= 100) return h->max;

    uint64_t target = (uint64_t)((p / 100.0) * (double)h->count + 0.5);
    if (target == 0) target = 1;
    uint64_t cum = 0;
    for (int i = 0; i < HIST_BUCKETS; i++)
    {
        cum += h->buckets[i];
        if (cum >= target)
        {
            uint64_t v = hist_value_at(i);

            if (v < h->min) v = h->min;
            if (v > h->max) v = h->max;
            return v;
        }
    }
    return h->max;
}
