#ifndef HIST_H
#define HIST_H

#include <stddef.h>
#include <stdint.h>

#define HIST_SUB     64
#define HIST_OCT     40
#define HIST_BUCKETS (HIST_SUB * HIST_OCT)

typedef struct
{
    uint64_t buckets[HIST_BUCKETS];
    uint64_t count;
    uint64_t sum;
    uint64_t min;
    uint64_t max;
} hist;

void hist_reset(hist *h);
void hist_record(hist *h, uint64_t value);

void hist_merge(hist *dst, const hist *src);

uint64_t hist_percentile(const hist *h, double p);

#endif
