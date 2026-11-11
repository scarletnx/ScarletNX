/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx-shaderperf's statistics (stats.c): a sample's summary with outliers left out, and the comparison of two
   samples (a shader against the first one) */
#ifndef SNX_SHADERPERF_STATS_H
#define SNX_SHADERPERF_STATS_H
#include <stddef.h>
#include <stdint.h>

enum { SP_OUTLIERS_NONE, SP_OUTLIERS_IQR, SP_OUTLIERS_MAD };

typedef struct {
    size_t n, rejected; /* samples kept, and left out as outliers */
    double min, max, mean, stddev, sem, cv; /* cv: stddev / mean */
    double median, mad;                     /* mad scaled by 1.4826, a normal sample's stddev */
    double p01, p05, p25, p75, p95, p99;
    double mean_lo, mean_hi;     /* 95 % confidence interval of the mean (Student's t) */
    double median_lo, median_hi; /* 95 % confidence interval of the median (order statistics) */
} SpStats;

/* x is sorted in place. The outliers by rule are left out of everything but rejected: IQR the values outside
   [Q1 - 1.5 IQR, Q3 + 1.5 IQR], MAD those whose modified z-score passes 3.5 */
void sp_stats(double* x, size_t n, int outliers, SpStats* out);
/* the values of x that sp_stats keeps under outliers, packed at the front of x (sorted). Their count */
size_t sp_stats_keep(double* x, size_t n, int outliers);

typedef struct {
    double ratio, ratio_lo, ratio_hi; /* median(b) / median(a), with a bootstrap 95 % interval */
    double mean_ratio;
    double welch_p;   /* Welch's t-test, two-sided */
    double mwu_p;     /* Mann-Whitney U, normal approximation with ties, two-sided */
    double cles;      /* probability that a value of b is greater than one of a (ties half) */
} SpCompare;

/* a and b are the kept samples; boot resamples (0 for 2000) from seed */
void sp_compare(const double* a, size_t na, const double* b, size_t nb, int boot, uint64_t seed, SpCompare* out);

/* a value of the splitmix64 sequence, which advances *state */
uint64_t sp_rand(uint64_t* state);
#endif
