/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Statistics for snx-shaderperf (stats.h) */
#include "stats.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

uint64_t sp_rand(uint64_t* state) {
    uint64_t z = (*state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static int cmp_double(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

/* the p quantile of sorted x, linear between ranks (type 7) */
static double quantile(const double* x, size_t n, double p) {
    if (!n)
        return 0;
    double h = (double)(n - 1) * p;
    size_t lo = (size_t)floor(h);
    size_t hi = lo + 1 < n ? lo + 1 : lo;
    return x[lo] + (h - (double)lo) * (x[hi] - x[lo]);
}

static double median_of(double* x, size_t n) {
    qsort(x, n, sizeof *x, cmp_double);
    return quantile(x, n, 0.5);
}

/* the scaled MAD of sorted x around median m */
static double mad_of(const double* x, size_t n, double m) {
    if (!n)
        return 0;
    double* d = malloc(n * sizeof *d);
    if (!d)
        return 0;
    for (size_t i = 0; i < n; i++)
        d[i] = fabs(x[i] - m);
    double r = median_of(d, n) * 1.4826;
    free(d);
    return r;
}

size_t sp_stats_keep(double* x, size_t n, int outliers) {
    qsort(x, n, sizeof *x, cmp_double);
    if (n < 4 || outliers == SP_OUTLIERS_NONE)
        return n;
    double lo = -INFINITY, hi = INFINITY;
    if (outliers == SP_OUTLIERS_IQR) {
        double q1 = quantile(x, n, 0.25), q3 = quantile(x, n, 0.75);
        lo = q1 - 1.5 * (q3 - q1);
        hi = q3 + 1.5 * (q3 - q1);
    } else {
        double m = quantile(x, n, 0.5), mad = mad_of(x, n, m);
        if (mad > 0) {
            /* modified z = 0.6745 (x - m) / MAD_raw = (x - m) / mad_scaled */
            lo = m - 3.5 * mad;
            hi = m + 3.5 * mad;
        }
    }
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (x[i] >= lo && x[i] <= hi)
            x[k++] = x[i];
    return k;
}

/* the 0.975 quantile of Student's t with df degrees of freedom (Cornish-Fisher, within 1e-3 from df 3 on) */
static double t975(double df) {
    static const double small[] = {12.706, 4.303, 3.182, 2.776, 2.571};
    if (df < 1)
        return NAN;
    if (df <= 5)
        return small[(int)df - 1];
    double z = 1.959964, z3 = z * z * z, z5 = z3 * z * z, z7 = z5 * z * z;
    return z + (z3 + z) / (4 * df) + (5 * z5 + 16 * z3 + 3 * z) / (96 * df * df) +
           (3 * z7 + 19 * z5 + 17 * z3 - 15 * z) / (384 * df * df * df);
}

void sp_stats(double* x, size_t n, int outliers, SpStats* out) {
    memset(out, 0, sizeof *out);
    size_t k = sp_stats_keep(x, n, outliers);
    out->n = k;
    out->rejected = n - k;
    if (!k)
        return;
    double sum = 0;
    for (size_t i = 0; i < k; i++)
        sum += x[i];
    out->mean = sum / (double)k;
    double ss = 0;
    for (size_t i = 0; i < k; i++)
        ss += (x[i] - out->mean) * (x[i] - out->mean);
    out->stddev = k > 1 ? sqrt(ss / (double)(k - 1)) : 0;
    out->sem = out->stddev / sqrt((double)k);
    out->cv = out->mean != 0 ? out->stddev / out->mean : 0;
    out->min = x[0];
    out->max = x[k - 1];
    out->median = quantile(x, k, 0.5);
    out->mad = mad_of(x, k, out->median);
    out->p01 = quantile(x, k, 0.01);
    out->p05 = quantile(x, k, 0.05);
    out->p25 = quantile(x, k, 0.25);
    out->p75 = quantile(x, k, 0.75);
    out->p95 = quantile(x, k, 0.95);
    out->p99 = quantile(x, k, 0.99);
    double t = k > 1 ? t975((double)(k - 1)) : 0;
    out->mean_lo = out->mean - t * out->sem;
    out->mean_hi = out->mean + t * out->sem;
    /* the ranks j and k of the order statistics around the median that cover it with 95 %: n/2 -+ 1.96 sqrt(n)/2 */
    double half = 1.959964 * sqrt((double)k) / 2;
    double jl = floor((double)k / 2 - half), jh = ceil((double)k / 2 + half);
    size_t lo = jl < 1 ? 0 : (size_t)jl - 1;
    size_t hi = jh > (double)k ? k - 1 : (size_t)jh - 1;
    out->median_lo = x[lo];
    out->median_hi = x[hi];
}

/* the regularized incomplete beta function I_x(a, b) by its continued fraction (modified Lentz) */
static double betacf(double a, double b, double x) {
    const double tiny = 1e-300;
    double qab = a + b, qap = a + 1, qam = a - 1, c = 1, d = 1 - qab * x / qap;
    if (fabs(d) < tiny)
        d = tiny;
    d = 1 / d;
    double h = d;
    for (int m = 1; m <= 300; m++) {
        double m2 = 2.0 * m;
        double aa = m * (b - m) * x / ((qam + m2) * (a + m2));
        d = 1 + aa * d;
        if (fabs(d) < tiny)
            d = tiny;
        c = 1 + aa / c;
        if (fabs(c) < tiny)
            c = tiny;
        d = 1 / d;
        h *= d * c;
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
        d = 1 + aa * d;
        if (fabs(d) < tiny)
            d = tiny;
        c = 1 + aa / c;
        if (fabs(c) < tiny)
            c = tiny;
        d = 1 / d;
        double del = d * c;
        h *= del;
        if (fabs(del - 1) < 1e-12)
            break;
    }
    return h;
}

static double ibeta(double a, double b, double x) {
    if (x <= 0)
        return 0;
    if (x >= 1)
        return 1;
    double front = exp(lgamma(a + b) - lgamma(a) - lgamma(b) + a * log(x) + b * log(1 - x));
    if (x < (a + 1) / (a + b + 2))
        return front * betacf(a, b, x) / a;
    return 1 - front * betacf(b, a, 1 - x) / b;
}

/* two-sided p of Student's t with df degrees of freedom */
static double t_p(double t, double df) {
    if (!isfinite(t))
        return t != t ? 1 : 0;
    return ibeta(df / 2, 0.5, df / (df + t * t));
}

static void mean_var(const double* x, size_t n, double* mean, double* var) {
    double s = 0;
    for (size_t i = 0; i < n; i++)
        s += x[i];
    *mean = n ? s / (double)n : 0;
    double ss = 0;
    for (size_t i = 0; i < n; i++)
        ss += (x[i] - *mean) * (x[i] - *mean);
    *var = n > 1 ? ss / (double)(n - 1) : 0;
}

typedef struct {
    double v;
    int group;
} Ranked;

static int cmp_ranked(const void* a, const void* b) { return cmp_double(&((const Ranked*)a)->v, &((const Ranked*)b)->v); }

void sp_compare(const double* a, size_t na, const double* b, size_t nb, int boot, uint64_t seed, SpCompare* out) {
    memset(out, 0, sizeof *out);
    out->welch_p = out->mwu_p = 1;
    if (!na || !nb)
        return;
    double ma, va, mb, vb;
    mean_var(a, na, &ma, &va);
    mean_var(b, nb, &mb, &vb);
    out->mean_ratio = ma != 0 ? mb / ma : 0;
    double* sa = malloc(na * sizeof *sa);
    double* sb = malloc(nb * sizeof *sb);
    Ranked* r = malloc((na + nb) * sizeof *r);
    if (!sa || !sb || !r) {
        free(sa);
        free(sb);
        free(r);
        return;
    }
    memcpy(sa, a, na * sizeof *sa);
    memcpy(sb, b, nb * sizeof *sb);
    double meda = median_of(sa, na), medb = median_of(sb, nb);
    out->ratio = meda != 0 ? medb / meda : 0;
    /* Welch */
    double se2 = va / (double)na + vb / (double)nb;
    if (se2 > 0 && na > 1 && nb > 1) {
        double t = (mb - ma) / sqrt(se2);
        double df = se2 * se2 / ((va / na) * (va / na) / (double)(na - 1) + (vb / nb) * (vb / nb) / (double)(nb - 1));
        out->welch_p = t_p(t, df);
    } else {
        out->welch_p = ma == mb ? 1 : 0;
    }
    /* Mann-Whitney: the ranks of a and b together, ties given their mean rank */
    size_t n = na + nb;
    for (size_t i = 0; i < na; i++)
        r[i] = (Ranked){a[i], 0};
    for (size_t i = 0; i < nb; i++)
        r[na + i] = (Ranked){b[i], 1};
    qsort(r, n, sizeof *r, cmp_ranked);
    double rank_b = 0, ties = 0;
    for (size_t i = 0; i < n;) {
        size_t j = i;
        while (j + 1 < n && r[j + 1].v == r[i].v)
            j++;
        double rank = (double)(i + j) / 2 + 1, t = (double)(j - i + 1);
        ties += t * t * t - t;
        for (size_t k = i; k <= j; k++)
            if (r[k].group)
                rank_b += rank;
        i = j + 1;
    }
    double ub = rank_b - (double)nb * (double)(nb + 1) / 2;
    double nn = (double)na * (double)nb;
    out->cles = ub / nn;
    double sigma = sqrt(nn / 12.0 * ((double)(n + 1) - ties / ((double)n * (double)(n - 1))));
    if (sigma > 0) {
        double z = (fabs(ub - nn / 2) - 0.5) / sigma;
        out->mwu_p = z <= 0 ? 1 : erfc(z / sqrt(2.0));
    }
    /* bootstrap of the ratio of medians */
    if (boot <= 0)
        boot = 2000;
    double* ratios = malloc((size_t)boot * sizeof *ratios);
    double* ra = malloc(na * sizeof *ra);
    double* rb = malloc(nb * sizeof *rb);
    if (ratios && ra && rb) {
        uint64_t st = seed ^ 0x5eed5eedull;
        for (int i = 0; i < boot; i++) {
            for (size_t k = 0; k < na; k++)
                ra[k] = a[sp_rand(&st) % na];
            for (size_t k = 0; k < nb; k++)
                rb[k] = b[sp_rand(&st) % nb];
            double m1 = median_of(ra, na), m2 = median_of(rb, nb);
            ratios[i] = m1 != 0 ? m2 / m1 : 0;
        }
        qsort(ratios, (size_t)boot, sizeof *ratios, cmp_double);
        out->ratio_lo = quantile(ratios, (size_t)boot, 0.025);
        out->ratio_hi = quantile(ratios, (size_t)boot, 0.975);
    }
    free(ratios);
    free(ra);
    free(rb);
    free(sa);
    free(sb);
    free(r);
}
