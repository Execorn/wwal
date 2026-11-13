// SPDX-License-Identifier: GPL-3.0-or-later

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

#include "waywal/we_mesh_cdt.h"

#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* =========================================================================
 * Shewchuk Adaptive-Precision Exact Geometric Predicates
 * ========================================================================= */

static const double c_splitter = 134217729.0; /* 2^27 + 1 */
static const double c_resulterrbound =
    (3.0 + 8.0 * 1.1102230246251565e-16) * 1.1102230246251565e-16;
static const double c_ccwerrbound_a =
    (3.0 + 16.0 * 1.1102230246251565e-16) * 1.1102230246251565e-16;
static const double c_ccwerrbound_b =
    (2.0 + 12.0 * 1.1102230246251565e-16) * 1.1102230246251565e-16;
static const double c_ccwerrbound_c =
    (9.0 + 64.0 * 1.1102230246251565e-16) * 1.1102230246251565e-16 * 1.1102230246251565e-16;
static const double c_iccerrbound_a =
    (10.0 + 96.0 * 1.1102230246251565e-16) * 1.1102230246251565e-16;
static const double c_iccerrbound_b =
    (4.0 + 48.0 * 1.1102230246251565e-16) * 1.1102230246251565e-16;
static const double c_iccerrbound_c =
    (44.0 + 576.0 * 1.1102230246251565e-16) * 1.1102230246251565e-16 * 1.1102230246251565e-16;

#define FAST_TWO_SUM_TAIL(a, b, x, y)                                                              \
    do {                                                                                           \
        double bvirt_ = (x) - (a);                                                                 \
        (y) = (b) - bvirt_;                                                                        \
    } while (0)

#define FAST_TWO_SUM(a, b, x, y)                                                                   \
    do {                                                                                           \
        (x) = (a) + (b);                                                                           \
        FAST_TWO_SUM_TAIL(a, b, x, y);                                                             \
    } while (0)

#define TWO_SUM_TAIL(a, b, x, y)                                                                   \
    do {                                                                                           \
        double bvirt_ = (x) - (a);                                                                 \
        double avirt_ = (x) - bvirt_;                                                              \
        double bround_ = (b) - bvirt_;                                                             \
        double around_ = (a) - avirt_;                                                             \
        (y) = around_ + bround_;                                                                   \
    } while (0)

#define TWO_SUM(a, b, x, y)                                                                        \
    do {                                                                                           \
        (x) = (a) + (b);                                                                           \
        TWO_SUM_TAIL(a, b, x, y);                                                                  \
    } while (0)

#define TWO_DIFF_TAIL(a, b, x, y)                                                                  \
    do {                                                                                           \
        double bvirt_ = (a) - (x);                                                                 \
        double avirt_ = (x) + bvirt_;                                                              \
        double bround_ = bvirt_ - (b);                                                             \
        double around_ = (a) - avirt_;                                                             \
        (y) = around_ + bround_;                                                                   \
    } while (0)

#define TWO_DIFF(a, b, x, y)                                                                       \
    do {                                                                                           \
        (x) = (a) - (b);                                                                           \
        TWO_DIFF_TAIL(a, b, x, y);                                                                 \
    } while (0)

#define TWO_PRODUCT(a, b, x, y)                                                                    \
    do {                                                                                           \
        (x) = (a) * (b);                                                                           \
        (y) = fma((a), (b), -(x));                                                                 \
    } while (0)

#define SPLIT_DOUBLE(a, ahi, alo)                                                                  \
    do {                                                                                           \
        double c_ = c_splitter * (a);                                                              \
        double abig_ = c_ - (a);                                                                   \
        (ahi) = c_ - abig_;                                                                        \
        (alo) = (a) - (ahi);                                                                       \
    } while (0)

#define TWO_PRODUCT_PRESPLIT(a, b, bhi, blo, x, y)                                                 \
    do {                                                                                           \
        (x) = (a) * (b);                                                                           \
        (y) = fma((a), (b), -(x));                                                                 \
        (void)(bhi);                                                                               \
        (void)(blo);                                                                               \
    } while (0)

#define TWO_ONE_DIFF(a1, a0, b, x2, x1, x0)                                                        \
    do {                                                                                           \
        double i_;                                                                                 \
        TWO_DIFF(a0, b, i_, x0);                                                                   \
        TWO_SUM(a1, i_, x2, x1);                                                                   \
    } while (0)

#define TWO_TWO_DIFF(a1, a0, b1, b0, x3, x2, x1, x0)                                               \
    do {                                                                                           \
        double j1_, j0_;                                                                           \
        TWO_ONE_DIFF(a1, a0, b0, j1_, j0_, x0);                                                    \
        TWO_ONE_DIFF(j1_, j0_, b1, x3, x2, x1);                                                    \
    } while (0)

static int fast_expansion_sum_zeroelim(int elen, const double *e, int flen, const double *f,
                                       double *h)
{
    double Q;
    double Qnew;
    double hh;
    int eindex = 0;
    int findex = 0;
    int hindex = 0;
    double enow = e[0];
    double fnow = f[0];

    if ((fnow > enow) == (fnow > -enow)) {
        Q = enow;
        eindex++;
        enow = (eindex < elen) ? e[eindex] : 0.0;
    } else {
        Q = fnow;
        findex++;
        fnow = (findex < flen) ? f[findex] : 0.0;
    }

    if ((eindex < elen) && (findex < flen)) {
        if ((fnow > enow) == (fnow > -enow)) {
            FAST_TWO_SUM(enow, Q, Qnew, hh);
            eindex++;
            enow = (eindex < elen) ? e[eindex] : 0.0;
        } else {
            FAST_TWO_SUM(fnow, Q, Qnew, hh);
            findex++;
            fnow = (findex < flen) ? f[findex] : 0.0;
        }
        Q = Qnew;
        if (hh != 0.0) {
            h[hindex++] = hh;
        }
        while ((eindex < elen) && (findex < flen)) {
            if ((fnow > enow) == (fnow > -enow)) {
                TWO_SUM(Q, enow, Qnew, hh);
                eindex++;
                enow = (eindex < elen) ? e[eindex] : 0.0;
            } else {
                TWO_SUM(Q, fnow, Qnew, hh);
                findex++;
                fnow = (findex < flen) ? f[findex] : 0.0;
            }
            Q = Qnew;
            if (hh != 0.0) {
                h[hindex++] = hh;
            }
        }
    }
    while (eindex < elen) {
        TWO_SUM(Q, enow, Qnew, hh);
        eindex++;
        enow = (eindex < elen) ? e[eindex] : 0.0;
        Q = Qnew;
        if (hh != 0.0) {
            h[hindex++] = hh;
        }
    }
    while (findex < flen) {
        TWO_SUM(Q, fnow, Qnew, hh);
        findex++;
        fnow = (findex < flen) ? f[findex] : 0.0;
        Q = Qnew;
        if (hh != 0.0) {
            h[hindex++] = hh;
        }
    }
    if ((Q != 0.0) || (hindex == 0)) {
        h[hindex++] = Q;
    }
    return hindex;
}

static int scale_expansion_zeroelim(int elen, const double *e, double b, double *h)
{
    double Q, sum, hh, product1, product0;
    double bhi, blo;
    SPLIT_DOUBLE(b, bhi, blo);

    TWO_PRODUCT_PRESPLIT(e[0], b, bhi, blo, Q, hh);
    int hindex = 0;
    if (hh != 0.0) {
        h[hindex++] = hh;
    }
    for (int i = 1; i < elen; i++) {
        double enow = e[i];
        TWO_PRODUCT_PRESPLIT(enow, b, bhi, blo, product1, product0);
        TWO_SUM(Q, product0, sum, hh);
        if (hh != 0.0) {
            h[hindex++] = hh;
        }
        FAST_TWO_SUM(product1, sum, Q, hh);
        if (hh != 0.0) {
            h[hindex++] = hh;
        }
    }
    if ((Q != 0.0) || (hindex == 0)) {
        h[hindex++] = Q;
    }
    return hindex;
}

static double estimate_expansion(int elen, const double *e)
{
    double Q = e[0];
    for (int i = 1; i < elen; i++) {
        Q += e[i];
    }
    return Q;
}

static double orient2d_adapt(we_cdt_vec2_t pa, we_cdt_vec2_t pb, we_cdt_vec2_t pc, double detsum)
{
    double acx = pa.x - pc.x;
    double acy = pa.y - pc.y;
    double bcx = pb.x - pc.x;
    double bcy = pb.y - pc.y;

    double detleft, detlefttail, detright, detrighttail;
    TWO_PRODUCT(acx, bcy, detleft, detlefttail);
    TWO_PRODUCT(acy, bcx, detright, detrighttail);

    double B[4];
    TWO_TWO_DIFF(detleft, detlefttail, detright, detrighttail, B[3], B[2], B[1], B[0]);

    double det = estimate_expansion(4, B);
    double errbound = c_ccwerrbound_b * detsum;
    if ((det >= errbound) || (-det >= errbound)) {
        return det;
    }

    double acxtail, acytail, bcxtail, bcytail;
    TWO_DIFF_TAIL(pa.x, pc.x, acx, acxtail);
    TWO_DIFF_TAIL(pb.x, pc.x, bcx, bcxtail);
    TWO_DIFF_TAIL(pa.y, pc.y, acy, acytail);
    TWO_DIFF_TAIL(pb.y, pc.y, bcy, bcytail);

    if ((acxtail == 0.0) && (acytail == 0.0) && (bcxtail == 0.0) && (bcytail == 0.0)) {
        return det;
    }

    errbound = c_ccwerrbound_c * detsum + c_resulterrbound * fabs(det);
    det += (acx * bcytail + bcy * acxtail) - (acy * bcxtail + bcx * acytail);
    if ((det >= errbound) || (-det >= errbound)) {
        return det;
    }

    double s1, s0, t1, t0;
    double u[4];
    double C1[8], C2[12], D[16];

    TWO_PRODUCT(acxtail, bcy, s1, s0);
    TWO_PRODUCT(acytail, bcx, t1, t0);
    TWO_TWO_DIFF(s1, s0, t1, t0, u[3], u[2], u[1], u[0]);
    int C1length = fast_expansion_sum_zeroelim(4, B, 4, u, C1);

    TWO_PRODUCT(acx, bcytail, s1, s0);
    TWO_PRODUCT(acy, bcxtail, t1, t0);
    TWO_TWO_DIFF(s1, s0, t1, t0, u[3], u[2], u[1], u[0]);
    int C2length = fast_expansion_sum_zeroelim(C1length, C1, 4, u, C2);

    TWO_PRODUCT(acxtail, bcytail, s1, s0);
    TWO_PRODUCT(acytail, bcxtail, t1, t0);
    TWO_TWO_DIFF(s1, s0, t1, t0, u[3], u[2], u[1], u[0]);
    int Dlength = fast_expansion_sum_zeroelim(C2length, C2, 4, u, D);

    return D[Dlength - 1];
}

static double orient2d(we_cdt_vec2_t pa, we_cdt_vec2_t pb, we_cdt_vec2_t pc)
{
    if (!isfinite(pa.x) || !isfinite(pa.y) || !isfinite(pb.x) || !isfinite(pb.y) ||
        !isfinite(pc.x) || !isfinite(pc.y)) {
        return 0.0;
    }
    double detleft = (pa.x - pc.x) * (pb.y - pc.y);
    double detright = (pa.y - pc.y) * (pb.x - pc.x);
    double det = detleft - detright;
    if (!isfinite(det)) {
        return 0.0;
    }

    double detsum;
    if (detleft > 0.0) {
        if (detright <= 0.0) {
            return det;
        }
        detsum = detleft + detright;
    } else if (detleft < 0.0) {
        if (detright >= 0.0) {
            return det;
        }
        detsum = -detleft - detright;
    } else {
        return det;
    }

    double errbound = c_ccwerrbound_a * detsum;
    if ((det >= errbound) || (-det >= errbound)) {
        return det;
    }
    double res = orient2d_adapt(pa, pb, pc, detsum);
    return isfinite(res) ? res : 0.0;
}

static double incircle_adapt(we_cdt_vec2_t pa, we_cdt_vec2_t pb, we_cdt_vec2_t pc, we_cdt_vec2_t pd,
                             double permanent)
{
    double adx = pa.x - pd.x;
    double bdx = pb.x - pd.x;
    double cdx = pc.x - pd.x;
    double ady = pa.y - pd.y;
    double bdy = pb.y - pd.y;
    double cdy = pc.y - pd.y;

    double bdxcdy1, bdxcdy0, cdxbdy1, cdxbdy0;
    double cdxady1, cdxady0, adxcdy1, adxcdy0;
    double adxbdy1, adxbdy0, bdxady1, bdxady0;
    TWO_PRODUCT(bdx, cdy, bdxcdy1, bdxcdy0);
    TWO_PRODUCT(cdx, bdy, cdxbdy1, cdxbdy0);
    double bc[4];
    TWO_TWO_DIFF(bdxcdy1, bdxcdy0, cdxbdy1, cdxbdy0, bc[3], bc[2], bc[1], bc[0]);

    TWO_PRODUCT(cdx, ady, cdxady1, cdxady0);
    TWO_PRODUCT(adx, cdy, adxcdy1, adxcdy0);
    double ca[4];
    TWO_TWO_DIFF(cdxady1, cdxady0, adxcdy1, adxcdy0, ca[3], ca[2], ca[1], ca[0]);

    TWO_PRODUCT(adx, bdy, adxbdy1, adxbdy0);
    TWO_PRODUCT(bdx, ady, bdxady1, bdxady0);
    double ab[4];
    TWO_TWO_DIFF(adxbdy1, adxbdy0, bdxady1, bdxady0, ab[3], ab[2], ab[1], ab[0]);

    double axbc[8], axxbc[16], aybc[8], ayybc[16], adet[32];
    int axbclen = scale_expansion_zeroelim(4, bc, adx, axbc);
    int axxbclen = scale_expansion_zeroelim(axbclen, axbc, adx, axxbc);
    int aybclen = scale_expansion_zeroelim(4, bc, ady, aybc);
    int ayybclen = scale_expansion_zeroelim(aybclen, aybc, ady, ayybc);
    int alen = fast_expansion_sum_zeroelim(axxbclen, axxbc, ayybclen, ayybc, adet);

    double bxca[8], bxxca[16], byca[8], byyca[16], bdet[32];
    int bxcalen = scale_expansion_zeroelim(4, ca, bdx, bxca);
    int bxxcalen = scale_expansion_zeroelim(bxcalen, bxca, bdx, bxxca);
    int bycalen = scale_expansion_zeroelim(4, ca, bdy, byca);
    int byycalen = scale_expansion_zeroelim(bycalen, byca, bdy, byyca);
    int blen = fast_expansion_sum_zeroelim(bxxcalen, bxxca, byycalen, byyca, bdet);

    double cxab[8], cxxab[16], cyab[8], cyyab[16], cdet[32];
    int cxablen = scale_expansion_zeroelim(4, ab, cdx, cxab);
    int cxxablen = scale_expansion_zeroelim(cxablen, cxab, cdx, cxxab);
    int cyablen = scale_expansion_zeroelim(4, ab, cdy, cyab);
    int cyyablen = scale_expansion_zeroelim(cyablen, cyab, cdy, cyyab);
    int clen = fast_expansion_sum_zeroelim(cxxablen, cxxab, cyyablen, cyyab, cdet);

    double abdet[64], fin1[1152], fin2[1152];
    int ablen = fast_expansion_sum_zeroelim(alen, adet, blen, bdet, abdet);
    int finlength = fast_expansion_sum_zeroelim(ablen, abdet, clen, cdet, fin1);

    double det = estimate_expansion(finlength, fin1);
    double errbound = c_iccerrbound_b * permanent;
    if ((det >= errbound) || (-det >= errbound)) {
        return det;
    }

    double adxtail, adytail, bdxtail, bdytail, cdxtail, cdytail;
    TWO_DIFF_TAIL(pa.x, pd.x, adx, adxtail);
    TWO_DIFF_TAIL(pa.y, pd.y, ady, adytail);
    TWO_DIFF_TAIL(pb.x, pd.x, bdx, bdxtail);
    TWO_DIFF_TAIL(pb.y, pd.y, bdy, bdytail);
    TWO_DIFF_TAIL(pc.x, pd.x, cdx, cdxtail);
    TWO_DIFF_TAIL(pc.y, pd.y, cdy, cdytail);

    if ((adxtail == 0.0) && (bdxtail == 0.0) && (cdxtail == 0.0) && (adytail == 0.0) &&
        (bdytail == 0.0) && (cdytail == 0.0)) {
        return det;
    }

    errbound = c_iccerrbound_c * permanent + c_resulterrbound * fabs(det);
    det += ((adx * adx + ady * ady) *
                ((bdx * cdytail + cdy * bdxtail) - (bdy * cdxtail + cdx * bdytail)) +
            2.0 * (adx * adxtail + ady * adytail) * (bdx * cdy - bdy * cdx)) +
           ((bdx * bdx + bdy * bdy) *
                ((cdx * adytail + ady * cdxtail) - (cdy * adxtail + adx * cdytail)) +
            2.0 * (bdx * bdxtail + bdy * bdytail) * (cdx * ady - cdy * adx)) +
           ((cdx * cdx + cdy * cdy) *
                ((adx * bdytail + bdy * adxtail) - (ady * bdxtail + bdx * adytail)) +
            2.0 * (cdx * cdxtail + cdy * cdytail) * (adx * bdy - ady * bdx));
    if ((det >= errbound) || (-det >= errbound)) {
        return det;
    }

    double *finnow = fin1;
    double *finother = fin2;

    double aa[4], bb[4], cc[4];
    int aalen = 0, bblen = 0, cclen = 0;
    if ((bdxtail != 0.0) || (bdytail != 0.0) || (cdxtail != 0.0) || (cdytail != 0.0)) {
        double adxadx1, adxadx0, adyady1, adyady0;
        TWO_PRODUCT(adx, adx, adxadx1, adxadx0);
        TWO_PRODUCT(ady, ady, adyady1, adyady0);
        double temp4a[4] = {adxadx0, adxadx1};
        double temp4b[4] = {adyady0, adyady1};
        aalen = fast_expansion_sum_zeroelim(2, temp4a, 2, temp4b, aa);
    }
    if ((cdxtail != 0.0) || (cdytail != 0.0) || (adxtail != 0.0) || (adytail != 0.0)) {
        double bdxbdx1, bdxbdx0, bdybdy1, bdybdy0;
        TWO_PRODUCT(bdx, bdx, bdxbdx1, bdxbdx0);
        TWO_PRODUCT(bdy, bdy, bdybdy1, bdybdy0);
        double temp4a[4] = {bdxbdx0, bdxbdx1};
        double temp4b[4] = {bdybdy0, bdybdy1};
        bblen = fast_expansion_sum_zeroelim(2, temp4a, 2, temp4b, bb);
    }
    if ((adxtail != 0.0) || (adytail != 0.0) || (bdxtail != 0.0) || (bdytail != 0.0)) {
        double cdxcdx1, cdxcdx0, cdycdy1, cdycdy0;
        TWO_PRODUCT(cdx, cdx, cdxcdx1, cdxcdx0);
        TWO_PRODUCT(cdy, cdy, cdycdy1, cdycdy0);
        double temp4a[4] = {cdxcdx0, cdxcdx1};
        double temp4b[4] = {cdycdy0, cdycdy1};
        cclen = fast_expansion_sum_zeroelim(2, temp4a, 2, temp4b, cc);
    }

    double temp8[8], temp16a[16], temp16b[16], temp16c[16];
    double temp32a[32], temp32b[32], temp48[48], temp64[64];
    int temp8len, temp16alen, temp16blen, temp16clen, temp32alen, temp32blen, temp48len, temp64len;

    if (adxtail != 0.0) {
        double axtbc[8];
        int axtbclen = scale_expansion_zeroelim(4, bc, adxtail, axtbc);
        temp16alen = scale_expansion_zeroelim(axtbclen, axtbc, 2.0 * adx, temp16a);

        double axtcc[8];
        int axtcclen = scale_expansion_zeroelim(cclen, cc, adxtail, axtcc);
        temp16blen = scale_expansion_zeroelim(axtcclen, axtcc, bdy, temp16b);

        double axtbb[8];
        int axtbblen = scale_expansion_zeroelim(bblen, bb, adxtail, axtbb);
        temp16clen = scale_expansion_zeroelim(axtbblen, axtbb, -cdy, temp16c);

        temp32alen = fast_expansion_sum_zeroelim(temp16alen, temp16a, temp16blen, temp16b, temp32a);
        temp48len = fast_expansion_sum_zeroelim(temp16clen, temp16c, temp32alen, temp32a, temp48);
        finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
        double *sw = finnow;
        finnow = finother;
        finother = sw;
    }
    if (adytail != 0.0) {
        double aytbc[8];
        int aytbclen = scale_expansion_zeroelim(4, bc, adytail, aytbc);
        temp16alen = scale_expansion_zeroelim(aytbclen, aytbc, 2.0 * ady, temp16a);

        double aytbb[8];
        int aytbblen = scale_expansion_zeroelim(bblen, bb, adytail, aytbb);
        temp16blen = scale_expansion_zeroelim(aytbblen, aytbb, cdx, temp16b);

        double aytcc[8];
        int aytcclen = scale_expansion_zeroelim(cclen, cc, adytail, aytcc);
        temp16clen = scale_expansion_zeroelim(aytcclen, aytcc, -bdx, temp16c);

        temp32alen = fast_expansion_sum_zeroelim(temp16alen, temp16a, temp16blen, temp16b, temp32a);
        temp48len = fast_expansion_sum_zeroelim(temp16clen, temp16c, temp32alen, temp32a, temp48);
        finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
        double *sw = finnow;
        finnow = finother;
        finother = sw;
    }
    if (bdxtail != 0.0) {
        double bxtca[8];
        int bxtcalen = scale_expansion_zeroelim(4, ca, bdxtail, bxtca);
        temp16alen = scale_expansion_zeroelim(bxtcalen, bxtca, 2.0 * bdx, temp16a);

        double bxtaa[8];
        int bxtaalen = scale_expansion_zeroelim(aalen, aa, bdxtail, bxtaa);
        temp16blen = scale_expansion_zeroelim(bxtaalen, bxtaa, cdy, temp16b);

        double bxtcc[8];
        int bxtcclen = scale_expansion_zeroelim(cclen, cc, bdxtail, bxtcc);
        temp16clen = scale_expansion_zeroelim(bxtcclen, bxtcc, -ady, temp16c);

        temp32alen = fast_expansion_sum_zeroelim(temp16alen, temp16a, temp16blen, temp16b, temp32a);
        temp48len = fast_expansion_sum_zeroelim(temp16clen, temp16c, temp32alen, temp32a, temp48);
        finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
        double *sw = finnow;
        finnow = finother;
        finother = sw;
    }
    if (bdytail != 0.0) {
        double bytca[8];
        int bytcalen = scale_expansion_zeroelim(4, ca, bdytail, bytca);
        temp16alen = scale_expansion_zeroelim(bytcalen, bytca, 2.0 * bdy, temp16a);

        double bytcc[8];
        int bytcclen = scale_expansion_zeroelim(cclen, cc, bdytail, bytcc);
        temp16blen = scale_expansion_zeroelim(bytcclen, bytcc, adx, temp16b);

        double bytaa[8];
        int bytaalen = scale_expansion_zeroelim(aalen, aa, bdytail, bytaa);
        temp16clen = scale_expansion_zeroelim(bytaalen, bytaa, -cdx, temp16c);

        temp32alen = fast_expansion_sum_zeroelim(temp16alen, temp16a, temp16blen, temp16b, temp32a);
        temp48len = fast_expansion_sum_zeroelim(temp16clen, temp16c, temp32alen, temp32a, temp48);
        finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
        double *sw = finnow;
        finnow = finother;
        finother = sw;
    }
    if (cdxtail != 0.0) {
        double cxtab[8];
        int cxtablen = scale_expansion_zeroelim(4, ab, cdxtail, cxtab);
        temp16alen = scale_expansion_zeroelim(cxtablen, cxtab, 2.0 * cdx, temp16a);

        double cxtbb[8];
        int cxtbblen = scale_expansion_zeroelim(bblen, bb, cdxtail, cxtbb);
        temp16blen = scale_expansion_zeroelim(cxtbblen, cxtbb, ady, temp16b);

        double cxtaa[8];
        int cxtaalen = scale_expansion_zeroelim(aalen, aa, cdxtail, cxtaa);
        temp16clen = scale_expansion_zeroelim(cxtaalen, cxtaa, -bdy, temp16c);

        temp32alen = fast_expansion_sum_zeroelim(temp16alen, temp16a, temp16blen, temp16b, temp32a);
        temp48len = fast_expansion_sum_zeroelim(temp16clen, temp16c, temp32alen, temp32a, temp48);
        finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
        double *sw = finnow;
        finnow = finother;
        finother = sw;
    }
    if (cdytail != 0.0) {
        double cytab[8];
        int cytablen = scale_expansion_zeroelim(4, ab, cdytail, cytab);
        temp16alen = scale_expansion_zeroelim(cytablen, cytab, 2.0 * cdy, temp16a);

        double cytaa[8];
        int cytaalen = scale_expansion_zeroelim(aalen, aa, cdytail, cytaa);
        temp16blen = scale_expansion_zeroelim(cytaalen, cytaa, bdx, temp16b);

        double cytbb[8];
        int cytbblen = scale_expansion_zeroelim(bblen, bb, cdytail, cytbb);
        temp16clen = scale_expansion_zeroelim(cytbblen, cytbb, -adx, temp16c);

        temp32alen = fast_expansion_sum_zeroelim(temp16alen, temp16a, temp16blen, temp16b, temp32a);
        temp48len = fast_expansion_sum_zeroelim(temp16clen, temp16c, temp32alen, temp32a, temp48);
        finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
        double *sw = finnow;
        finnow = finother;
        finother = sw;
    }

    if ((adxtail != 0.0) || (adytail != 0.0)) {
        double bct[8], bctt[4];
        int bctlen, bcttlen;
        if ((bdxtail != 0.0) || (bdytail != 0.0) || (cdxtail != 0.0) || (cdytail != 0.0)) {
            double s1, s0, t1, t0, u[4], v[4];
            TWO_PRODUCT(bdxtail, cdy, s1, s0);
            TWO_PRODUCT(bdytail, cdx, t1, t0);
            TWO_TWO_DIFF(s1, s0, t1, t0, u[3], u[2], u[1], u[0]);
            TWO_PRODUCT(bdx, cdytail, s1, s0);
            TWO_PRODUCT(bdy, cdxtail, t1, t0);
            TWO_TWO_DIFF(s1, s0, t1, t0, v[3], v[2], v[1], v[0]);
            bctlen = fast_expansion_sum_zeroelim(4, u, 4, v, bct);

            TWO_PRODUCT(bdxtail, cdytail, s1, s0);
            TWO_PRODUCT(bdytail, cdxtail, t1, t0);
            TWO_TWO_DIFF(s1, s0, t1, t0, bctt[3], bctt[2], bctt[1], bctt[0]);
            bcttlen = 4;
        } else {
            bct[0] = 0.0;
            bctlen = 1;
            bctt[0] = 0.0;
            bcttlen = 1;
        }

        if (adxtail != 0.0) {
            double axtbc[8];
            int axtbclen = scale_expansion_zeroelim(4, bc, adxtail, axtbc);
            temp16alen = scale_expansion_zeroelim(axtbclen, axtbc, adxtail, temp16a);
            temp32alen = scale_expansion_zeroelim(bctlen, bct, adxtail, temp32a);
            temp32blen = scale_expansion_zeroelim(temp32alen, temp32a, 2.0 * adx, temp32b);
            temp48len =
                fast_expansion_sum_zeroelim(temp16alen, temp16a, temp32blen, temp32b, temp48);
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
            double *sw = finnow;
            finnow = finother;
            finother = sw;

            if (bdytail != 0.0) {
                temp8len = scale_expansion_zeroelim(4, cc, adxtail, temp8);
                temp16alen = scale_expansion_zeroelim(temp8len, temp8, bdytail, temp16a);
                finlength =
                    fast_expansion_sum_zeroelim(finlength, finnow, temp16alen, temp16a, finother);
                sw = finnow;
                finnow = finother;
                finother = sw;
            }
            if (cdytail != 0.0) {
                temp8len = scale_expansion_zeroelim(4, bb, -adxtail, temp8);
                temp16alen = scale_expansion_zeroelim(temp8len, temp8, cdytail, temp16a);
                finlength =
                    fast_expansion_sum_zeroelim(finlength, finnow, temp16alen, temp16a, finother);
                sw = finnow;
                finnow = finother;
                finother = sw;
            }
            temp32alen = scale_expansion_zeroelim(bctlen, bct, adxtail, temp32a);
            temp64len = scale_expansion_zeroelim(temp32alen, temp32a, adxtail, temp64);
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp64len, temp64, finother);
            sw = finnow;
            finnow = finother;
            finother = sw;

            temp16alen = scale_expansion_zeroelim(bcttlen, bctt, adxtail, temp16a);
            temp32alen = scale_expansion_zeroelim(temp16alen, temp16a, 2.0 * adx, temp32a);
            temp16blen = scale_expansion_zeroelim(temp16alen, temp16a, adxtail, temp16b);
            temp48len =
                fast_expansion_sum_zeroelim(temp32alen, temp32a, temp16blen, temp16b, temp48);
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
            sw = finnow;
            finnow = finother;
            finother = sw;
        }
        if (adytail != 0.0) {
            double aytbc[8];
            int aytbclen = scale_expansion_zeroelim(4, bc, adytail, aytbc);
            temp16alen = scale_expansion_zeroelim(aytbclen, aytbc, adytail, temp16a);
            temp32alen = scale_expansion_zeroelim(bctlen, bct, adytail, temp32a);
            temp32blen = scale_expansion_zeroelim(temp32alen, temp32a, 2.0 * ady, temp32b);
            temp48len =
                fast_expansion_sum_zeroelim(temp16alen, temp16a, temp32blen, temp32b, temp48);
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
            double *sw = finnow;
            finnow = finother;
            finother = sw;

            temp32alen = scale_expansion_zeroelim(bctlen, bct, adytail, temp32a);
            temp64len = scale_expansion_zeroelim(temp32alen, temp32a, adytail, temp64);
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp64len, temp64, finother);
            sw = finnow;
            finnow = finother;
            finother = sw;

            temp16alen = scale_expansion_zeroelim(bcttlen, bctt, adytail, temp16a);
            temp32alen = scale_expansion_zeroelim(temp16alen, temp16a, 2.0 * ady, temp32a);
            temp16blen = scale_expansion_zeroelim(temp16alen, temp16a, adytail, temp16b);
            temp48len =
                fast_expansion_sum_zeroelim(temp32alen, temp32a, temp16blen, temp16b, temp48);
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
            sw = finnow;
            finnow = finother;
            finother = sw;
        }
    }

    if ((bdxtail != 0.0) || (bdytail != 0.0)) {
        double cat[8], catt[4];
        int catlen, cattlen;
        if ((cdxtail != 0.0) || (cdytail != 0.0) || (adxtail != 0.0) || (adytail != 0.0)) {
            double s1, s0, t1, t0, u[4], v[4];
            TWO_PRODUCT(cdxtail, ady, s1, s0);
            TWO_PRODUCT(cdytail, adx, t1, t0);
            TWO_TWO_DIFF(s1, s0, t1, t0, u[3], u[2], u[1], u[0]);
            TWO_PRODUCT(cdx, adytail, s1, s0);
            TWO_PRODUCT(cdy, adxtail, t1, t0);
            TWO_TWO_DIFF(s1, s0, t1, t0, v[3], v[2], v[1], v[0]);
            catlen = fast_expansion_sum_zeroelim(4, u, 4, v, cat);

            TWO_PRODUCT(cdxtail, adytail, s1, s0);
            TWO_PRODUCT(cdytail, adxtail, t1, t0);
            TWO_TWO_DIFF(s1, s0, t1, t0, catt[3], catt[2], catt[1], catt[0]);
            cattlen = 4;
        } else {
            cat[0] = 0.0;
            catlen = 1;
            catt[0] = 0.0;
            cattlen = 1;
        }

        if (bdxtail != 0.0) {
            double bxtca[8];
            int bxtcalen = scale_expansion_zeroelim(4, ca, bdxtail, bxtca);
            temp16alen = scale_expansion_zeroelim(bxtcalen, bxtca, bdxtail, temp16a);
            temp32alen = scale_expansion_zeroelim(catlen, cat, bdxtail, temp32a);
            temp32blen = scale_expansion_zeroelim(temp32alen, temp32a, 2.0 * bdx, temp32b);
            temp48len =
                fast_expansion_sum_zeroelim(temp16alen, temp16a, temp32blen, temp32b, temp48);
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
            double *sw = finnow;
            finnow = finother;
            finother = sw;

            if (cdytail != 0.0) {
                temp8len = scale_expansion_zeroelim(4, aa, bdxtail, temp8);
                temp16alen = scale_expansion_zeroelim(temp8len, temp8, cdytail, temp16a);
                finlength =
                    fast_expansion_sum_zeroelim(finlength, finnow, temp16alen, temp16a, finother);
                sw = finnow;
                finnow = finother;
                finother = sw;
            }
            if (adytail != 0.0) {
                temp8len = scale_expansion_zeroelim(4, cc, -bdxtail, temp8);
                temp16alen = scale_expansion_zeroelim(temp8len, temp8, adytail, temp16a);
                finlength =
                    fast_expansion_sum_zeroelim(finlength, finnow, temp16alen, temp16a, finother);
                sw = finnow;
                finnow = finother;
                finother = sw;
            }
            temp32alen = scale_expansion_zeroelim(catlen, cat, bdxtail, temp32a);
            temp64len = scale_expansion_zeroelim(temp32alen, temp32a, bdxtail, temp64);
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp64len, temp64, finother);
            sw = finnow;
            finnow = finother;
            finother = sw;

            temp16alen = scale_expansion_zeroelim(cattlen, catt, bdxtail, temp16a);
            temp32alen = scale_expansion_zeroelim(temp16alen, temp16a, 2.0 * bdx, temp32a);
            temp16blen = scale_expansion_zeroelim(temp16alen, temp16a, bdxtail, temp16b);
            temp48len =
                fast_expansion_sum_zeroelim(temp32alen, temp32a, temp16blen, temp16b, temp48);
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
            sw = finnow;
            finnow = finother;
            finother = sw;
        }
        if (bdytail != 0.0) {
            double bytca[8];
            int bytcalen = scale_expansion_zeroelim(4, ca, bdytail, bytca);
            temp16alen = scale_expansion_zeroelim(bytcalen, bytca, bdytail, temp16a);
            temp32alen = scale_expansion_zeroelim(catlen, cat, bdytail, temp32a);
            temp32blen = scale_expansion_zeroelim(temp32alen, temp32a, 2.0 * bdy, temp32b);
            temp48len =
                fast_expansion_sum_zeroelim(temp16alen, temp16a, temp32blen, temp32b, temp48);
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
            double *sw = finnow;
            finnow = finother;
            finother = sw;

            temp32alen = scale_expansion_zeroelim(catlen, cat, bdytail, temp32a);
            temp64len = scale_expansion_zeroelim(temp32alen, temp32a, bdytail, temp64);
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp64len, temp64, finother);
            sw = finnow;
            finnow = finother;
            finother = sw;

            temp16alen = scale_expansion_zeroelim(cattlen, catt, bdytail, temp16a);
            temp32alen = scale_expansion_zeroelim(temp16alen, temp16a, 2.0 * bdy, temp32a);
            temp16blen = scale_expansion_zeroelim(temp16alen, temp16a, bdytail, temp16b);
            temp48len =
                fast_expansion_sum_zeroelim(temp32alen, temp32a, temp16blen, temp16b, temp48);
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
            sw = finnow;
            finnow = finother;
            finother = sw;
        }
    }

    if ((cdxtail != 0.0) || (cdytail != 0.0)) {
        double abt[8], abtt[4];
        int abtlen, abttlen;
        if ((adxtail != 0.0) || (adytail != 0.0) || (bdxtail != 0.0) || (bdytail != 0.0)) {
            double s1, s0, t1, t0, u[4], v[4];
            TWO_PRODUCT(adxtail, bdy, s1, s0);
            TWO_PRODUCT(adytail, bdx, t1, t0);
            TWO_TWO_DIFF(s1, s0, t1, t0, u[3], u[2], u[1], u[0]);
            TWO_PRODUCT(adx, bdytail, s1, s0);
            TWO_PRODUCT(ady, bdxtail, t1, t0);
            TWO_TWO_DIFF(s1, s0, t1, t0, v[3], v[2], v[1], v[0]);
            abtlen = fast_expansion_sum_zeroelim(4, u, 4, v, abt);

            TWO_PRODUCT(adxtail, bdytail, s1, s0);
            TWO_PRODUCT(adytail, bdxtail, t1, t0);
            TWO_TWO_DIFF(s1, s0, t1, t0, abtt[3], abtt[2], abtt[1], abtt[0]);
            abttlen = 4;
        } else {
            abt[0] = 0.0;
            abtlen = 1;
            abtt[0] = 0.0;
            abttlen = 1;
        }

        if (cdxtail != 0.0) {
            double cxtab[8];
            int cxtablen = scale_expansion_zeroelim(4, ab, cdxtail, cxtab);
            temp16alen = scale_expansion_zeroelim(cxtablen, cxtab, cdxtail, temp16a);
            temp32alen = scale_expansion_zeroelim(abtlen, abt, cdxtail, temp32a);
            temp32blen = scale_expansion_zeroelim(temp32alen, temp32a, 2.0 * cdx, temp32b);
            temp48len =
                fast_expansion_sum_zeroelim(temp16alen, temp16a, temp32blen, temp32b, temp48);
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
            double *sw = finnow;
            finnow = finother;
            finother = sw;

            if (adytail != 0.0) {
                temp8len = scale_expansion_zeroelim(4, bb, cdxtail, temp8);
                temp16alen = scale_expansion_zeroelim(temp8len, temp8, adytail, temp16a);
                finlength =
                    fast_expansion_sum_zeroelim(finlength, finnow, temp16alen, temp16a, finother);
                sw = finnow;
                finnow = finother;
                finother = sw;
            }
            if (bdytail != 0.0) {
                temp8len = scale_expansion_zeroelim(4, aa, -cdxtail, temp8);
                temp16alen = scale_expansion_zeroelim(temp8len, temp8, bdytail, temp16a);
                finlength =
                    fast_expansion_sum_zeroelim(finlength, finnow, temp16alen, temp16a, finother);
                sw = finnow;
                finnow = finother;
                finother = sw;
            }
            temp32alen = scale_expansion_zeroelim(abtlen, abt, cdxtail, temp32a);
            temp64len = scale_expansion_zeroelim(temp32alen, temp32a, cdxtail, temp64);
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp64len, temp64, finother);
            sw = finnow;
            finnow = finother;
            finother = sw;

            temp16alen = scale_expansion_zeroelim(abttlen, abtt, cdxtail, temp16a);
            temp32alen = scale_expansion_zeroelim(temp16alen, temp16a, 2.0 * cdx, temp32a);
            temp16blen = scale_expansion_zeroelim(temp16alen, temp16a, cdxtail, temp16b);
            temp48len =
                fast_expansion_sum_zeroelim(temp32alen, temp32a, temp16blen, temp16b, temp48);
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
            sw = finnow;
            finnow = finother;
            finother = sw;
        }
        if (cdytail != 0.0) {
            double cytab[8];
            int cytablen = scale_expansion_zeroelim(4, ab, cdytail, cytab);
            temp16alen = scale_expansion_zeroelim(cytablen, cytab, cdytail, temp16a);
            temp32alen = scale_expansion_zeroelim(abtlen, abt, cdytail, temp32a);
            temp32blen = scale_expansion_zeroelim(temp32alen, temp32a, 2.0 * cdy, temp32b);
            temp48len =
                fast_expansion_sum_zeroelim(temp16alen, temp16a, temp32blen, temp32b, temp48);
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
            double *sw = finnow;
            finnow = finother;
            finother = sw;

            temp32alen = scale_expansion_zeroelim(abtlen, abt, cdytail, temp32a);
            temp64len = scale_expansion_zeroelim(temp32alen, temp32a, cdytail, temp64);
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp64len, temp64, finother);
            sw = finnow;
            finnow = finother;
            finother = sw;

            temp16alen = scale_expansion_zeroelim(abttlen, abtt, cdytail, temp16a);
            temp32alen = scale_expansion_zeroelim(temp16alen, temp16a, 2.0 * cdy, temp32a);
            temp16blen = scale_expansion_zeroelim(temp16alen, temp16a, cdytail, temp16b);
            temp48len =
                fast_expansion_sum_zeroelim(temp32alen, temp32a, temp16blen, temp16b, temp48);
            finlength = fast_expansion_sum_zeroelim(finlength, finnow, temp48len, temp48, finother);
            sw = finnow;
            finnow = finother;
            finother = sw;
        }
    }

    return finnow[finlength - 1];
}

static double incircle(we_cdt_vec2_t pa, we_cdt_vec2_t pb, we_cdt_vec2_t pc, we_cdt_vec2_t pd)
{
    if (!isfinite(pa.x) || !isfinite(pa.y) || !isfinite(pb.x) || !isfinite(pb.y) ||
        !isfinite(pc.x) || !isfinite(pc.y) || !isfinite(pd.x) || !isfinite(pd.y)) {
        return 0.0;
    }
    double adx = pa.x - pd.x;
    double bdx = pb.x - pd.x;
    double cdx = pc.x - pd.x;
    double ady = pa.y - pd.y;
    double bdy = pb.y - pd.y;
    double cdy = pc.y - pd.y;

    double bdxcdy = bdx * cdy;
    double cdxbdy = cdx * bdy;
    double alift = adx * adx + ady * ady;

    double cdxady = cdx * ady;
    double adxcdy = adx * cdy;
    double blift = bdx * bdx + bdy * bdy;

    double adxbdy = adx * bdy;
    double bdxady = bdx * ady;
    double clift = cdx * cdx + cdy * cdy;

    double det = alift * (bdxcdy - cdxbdy) + blift * (cdxady - adxcdy) + clift * (adxbdy - bdxady);
    double permanent = (fabs(bdxcdy) + fabs(cdxbdy)) * alift +
                       (fabs(cdxady) + fabs(adxcdy)) * blift +
                       (fabs(adxbdy) + fabs(bdxady)) * clift;
    if (!isfinite(det) || !isfinite(permanent)) {
        return 0.0;
    }
    double errbound = c_iccerrbound_a * permanent;
    if ((det > errbound) || (-det > errbound)) {
        return det;
    }

    double res = incircle_adapt(pa, pb, pc, pd, permanent);
    return isfinite(res) ? res : 0.0;
}

/* =========================================================================
 * Configuration & Numeric Helpers
 * ========================================================================= */

static float clamp_f32(float v, float lo, float hi, float def_v)
{
    if (!isfinite(v)) {
        return def_v;
    }
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static double clamp_f64(double v, double lo, double hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

void we_mesh_gen_cfg_default(we_mesh_gen_cfg_t *out_cfg)
{
    if (!out_cfg) {
        return;
    }
    out_cfg->alpha_threshold = 0.5f;
    out_cfg->dilation_px = 2.0f;
    out_cfg->simplify_eps_px = 1.5f;
    out_cfg->interior_spacing = 24.0f;
    out_cfg->min_angle_deg = 25.0f;
    out_cfg->min_island_area = 4.0f;
    out_cfg->max_vertices = 8192u;
}

static bool sanitize_cfg(const we_mesh_gen_cfg_t *cfg, we_mesh_gen_cfg_t *out)
{
    if (!cfg) {
        we_mesh_gen_cfg_default(out);
        return true;
    }
    out->alpha_threshold = clamp_f32(cfg->alpha_threshold, 1.0f / 255.0f, 1.0f, 0.5f);
    out->dilation_px = clamp_f32(cfg->dilation_px, 0.0f, WE_CDT_MAX_DILATION_PX, 2.0f);
    out->simplify_eps_px = clamp_f32(cfg->simplify_eps_px, 0.25f, 64.0f, 1.5f);
    out->interior_spacing = clamp_f32(cfg->interior_spacing, 4.0f, 512.0f, 24.0f);
    out->min_angle_deg =
        clamp_f32(cfg->min_angle_deg, WE_CDT_MIN_ANGLE_MIN_DEG, WE_CDT_MIN_ANGLE_MAX_DEG, 25.0f);
    out->min_island_area = clamp_f32(cfg->min_island_area, 0.0f, 4096.0f, 4.0f);
    if (cfg->max_vertices == 0u) {
        out->max_vertices = 8192u;
    } else if (cfg->max_vertices < 3u) {
        return false;
    } else if (cfg->max_vertices > WE_MDL_MAX_VERTICES) {
        out->max_vertices = WE_MDL_MAX_VERTICES;
    } else {
        out->max_vertices = cfg->max_vertices;
    }
    return true;
}

static double dist2_pts(we_cdt_vec2_t a, we_cdt_vec2_t b)
{
    double dx = a.x - b.x;
    double dy = a.y - b.y;
    return dx * dx + dy * dy;
}

static double dist2_point_seg(we_cdt_vec2_t p, we_cdt_vec2_t a, we_cdt_vec2_t b)
{
    double vx = b.x - a.x;
    double vy = b.y - a.y;
    double l2 = vx * vx + vy * vy;
    if (l2 <= 1e-24) {
        return dist2_pts(p, a);
    }
    double t = ((p.x - a.x) * vx + (p.y - a.y) * vy) / l2;
    t = clamp_f64(t, 0.0, 1.0);
    double px = a.x + t * vx;
    double py = a.y + t * vy;
    double dx = p.x - px;
    double dy = p.y - py;
    return dx * dx + dy * dy;
}

static double compute_signed_area(const we_cdt_vec2_t *pts, uint32_t n)
{
    if (!pts || n < 3u) {
        return 0.0;
    }
    double ox = pts[0].x;
    double oy = pts[0].y;
    double sum = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t j = (i + 1u < n) ? (i + 1u) : 0u;
        double x0 = pts[i].x - ox;
        double y0 = pts[i].y - oy;
        double x1 = pts[j].x - ox;
        double y1 = pts[j].y - oy;
        sum += x0 * y1 - x1 * y0;
    }
    return 0.5 * sum;
}

static bool point_on_segment_exact(we_cdt_vec2_t p, we_cdt_vec2_t a, we_cdt_vec2_t b)
{
    if (orient2d(a, b, p) != 0.0) {
        return false;
    }
    double min_x = fmin(a.x, b.x);
    double max_x = fmax(a.x, b.x);
    double min_y = fmin(a.y, b.y);
    double max_y = fmax(a.y, b.y);
    return (p.x >= min_x && p.x <= max_x && p.y >= min_y && p.y <= max_y);
}

static bool segments_intersect_or_touch(we_cdt_vec2_t a, we_cdt_vec2_t b, we_cdt_vec2_t c,
                                        we_cdt_vec2_t d)
{
    if (fmax(a.x, b.x) < fmin(c.x, d.x) || fmax(c.x, d.x) < fmin(a.x, b.x) ||
        fmax(a.y, b.y) < fmin(c.y, d.y) || fmax(c.y, d.y) < fmin(a.y, b.y)) {
        return false;
    }
    double o1 = orient2d(a, b, c);
    double o2 = orient2d(a, b, d);
    double o3 = orient2d(c, d, a);
    double o4 = orient2d(c, d, b);

    if (((o1 > 0.0 && o2 < 0.0) || (o1 < 0.0 && o2 > 0.0)) &&
        ((o3 > 0.0 && o4 < 0.0) || (o3 < 0.0 && o4 > 0.0))) {
        return true;
    }
    if (o1 == 0.0 && point_on_segment_exact(c, a, b)) {
        return true;
    }
    if (o2 == 0.0 && point_on_segment_exact(d, a, b)) {
        return true;
    }
    if (o3 == 0.0 && point_on_segment_exact(a, c, d)) {
        return true;
    }
    if (o4 == 0.0 && point_on_segment_exact(b, c, d)) {
        return true;
    }
    return false;
}

static bool point_in_or_on_polygon(we_cdt_vec2_t p, const we_cdt_vec2_t *poly, uint32_t n)
{
    int wn = 0;
    for (uint32_t i = 0; i < n; i++) {
        we_cdt_vec2_t a = poly[i];
        we_cdt_vec2_t b = poly[(i + 1u < n) ? (i + 1u) : 0u];
        if (point_on_segment_exact(p, a, b)) {
            return true;
        }
        if (a.y <= p.y) {
            if (b.y > p.y && orient2d(a, b, p) > 0.0) {
                wn++;
            }
        } else {
            if (b.y <= p.y && orient2d(a, b, p) < 0.0) {
                wn--;
            }
        }
    }
    return wn != 0;
}

/* =========================================================================
 * Step 1: Monotonic-Deque Dilation & Asymptotic Decider Marching Squares
 * ========================================================================= */

static void dilate_1d_max(const uint8_t *src, uint8_t *dst, uint32_t len, ptrdiff_t stride,
                          uint32_t radius, uint32_t *dq_buf)
{
    uint32_t cap = 2u * radius + 4u;
    uint32_t head = 0u;
    uint32_t tail = 0u;
    uint32_t prefill = (radius < len) ? radius : len;

    for (uint32_t j = 0u; j < prefill; j++) {
        uint8_t val = src[(ptrdiff_t)j * stride];
        while (head != tail) {
            uint32_t back = (tail + cap - 1u) % cap;
            if (src[(ptrdiff_t)dq_buf[back] * stride] <= val) {
                tail = back;
            } else {
                break;
            }
        }
        dq_buf[tail] = j;
        tail = (tail + 1u) % cap;
    }

    for (uint32_t k = 0u; k < len; k++) {
        uint32_t hi = k + radius;
        if (hi < len) {
            uint8_t val = src[(ptrdiff_t)hi * stride];
            while (head != tail) {
                uint32_t back = (tail + cap - 1u) % cap;
                if (src[(ptrdiff_t)dq_buf[back] * stride] <= val) {
                    tail = back;
                } else {
                    break;
                }
            }
            dq_buf[tail] = hi;
            tail = (tail + 1u) % cap;
        }
        if (head != tail && dq_buf[head] + radius < k) {
            head = (head + 1u) % cap;
        }
        dst[(ptrdiff_t)k * stride] = src[(ptrdiff_t)dq_buf[head] * stride];
    }
}

typedef enum {
    MS_SIDE_TOP = 0,
    MS_SIDE_RIGHT = 1,
    MS_SIDE_BOTTOM = 2,
    MS_SIDE_LEFT = 3,
} ms_side_t;

static size_t ms_edge_id(uint32_t ci, uint32_t cj, ms_side_t side, uint32_t wp)
{
    switch (side) {
    case MS_SIDE_TOP:
        return ((size_t)cj * wp + ci) * 2u;
    case MS_SIDE_BOTTOM:
        return ((size_t)(cj + 1u) * wp + ci) * 2u;
    case MS_SIDE_LEFT:
        return ((size_t)cj * wp + ci) * 2u + 1u;
    case MS_SIDE_RIGHT:
    default:
        return ((size_t)cj * wp + (ci + 1u)) * 2u + 1u;
    }
}

static we_cdt_vec2_t ms_crossing_pos(uint32_t ci, uint32_t cj, ms_side_t side, uint32_t pad_border,
                                     int32_t s00, int32_t s10, int32_t s11, int32_t s01)
{
    double bx = (double)ci - (double)pad_border + 0.5;
    double by = (double)cj - (double)pad_border + 0.5;
    double t = 0.5;
    switch (side) {
    case MS_SIDE_TOP:
        t = (double)(-s00) / (double)(s10 - s00);
        return (we_cdt_vec2_t){bx + t, by};
    case MS_SIDE_BOTTOM:
        t = (double)(-s01) / (double)(s11 - s01);
        return (we_cdt_vec2_t){bx + t, by + 1.0};
    case MS_SIDE_LEFT:
        t = (double)(-s00) / (double)(s01 - s00);
        return (we_cdt_vec2_t){bx, by + t};
    case MS_SIDE_RIGHT:
    default:
        t = (double)(-s10) / (double)(s11 - s10);
        return (we_cdt_vec2_t){bx + 1.0, by + t};
    }
}

static ms_side_t ms_cell_exit_side(int32_t s00, int32_t s10, int32_t s11, int32_t s01, uint8_t mask,
                                   ms_side_t enter_side)
{
    switch (mask) {
    case 1:
        return MS_SIDE_LEFT;
    case 2:
        return MS_SIDE_TOP;
    case 3:
        return MS_SIDE_LEFT;
    case 4:
        return MS_SIDE_RIGHT;
    case 6:
        return MS_SIDE_TOP;
    case 7:
        return MS_SIDE_LEFT;
    case 8:
        return MS_SIDE_BOTTOM;
    case 9:
        return MS_SIDE_BOTTOM;
    case 11:
        return MS_SIDE_BOTTOM;
    case 12:
        return MS_SIDE_RIGHT;
    case 13:
        return MS_SIDE_RIGHT;
    case 14:
        return MS_SIDE_TOP;
    case 5: {
        int64_t det = (int64_t)s00 * s11 - (int64_t)s10 * s01;
        if (det > 0) {
            return (enter_side == MS_SIDE_TOP) ? MS_SIDE_RIGHT : MS_SIDE_LEFT;
        }
        return (enter_side == MS_SIDE_TOP) ? MS_SIDE_LEFT : MS_SIDE_RIGHT;
    }
    case 10: {
        int64_t det = (int64_t)s00 * s11 - (int64_t)s10 * s01;
        if (det < 0) {
            return (enter_side == MS_SIDE_LEFT) ? MS_SIDE_TOP : MS_SIDE_BOTTOM;
        }
        return (enter_side == MS_SIDE_RIGHT) ? MS_SIDE_TOP : MS_SIDE_BOTTOM;
    }
    default:
        return enter_side;
    }
}

static bool bitset_test_and_set(uint8_t *bits, size_t idx)
{
    size_t byte_idx = idx >> 3u;
    uint8_t bit_mask = (uint8_t)(1u << (idx & 7u));
    bool was_set = (bits[byte_idx] & bit_mask) != 0u;
    bits[byte_idx] |= bit_mask;
    return was_set;
}

#define WE_CDT_MAX_WORK_DIM 2048u

bool we_mesh_extract_contours(const uint8_t *alpha, uint32_t width, uint32_t height,
                              uint32_t stride_bytes, const we_mesh_gen_cfg_t *cfg,
                              we_cdt_loop_t **out_loops, uint32_t *out_loop_count)
{
    if (out_loops) {
        *out_loops = NULL;
    }
    if (out_loop_count) {
        *out_loop_count = 0u;
    }
    if (!alpha || !out_loops || !out_loop_count) {
        return false;
    }
    if (width == 0u || height == 0u || width > WE_CDT_MAX_IMAGE_DIM ||
        height > WE_CDT_MAX_IMAGE_DIM) {
        return false;
    }
    if ((uint64_t)width * (uint64_t)height > (uint64_t)WE_CDT_MAX_IMAGE_PIXELS) {
        return false;
    }
    if (stride_bytes == 0u) {
        stride_bytes = width;
    } else if (stride_bytes < width) {
        return false;
    }

    we_mesh_gen_cfg_t s_cfg;
    if (!sanitize_cfg(cfg, &s_cfg)) {
        return false;
    }

    /* Cap the working raster so 4K/8K textures avoid transient 150+ MB heap spikes. */
    uint32_t work_w = width;
    uint32_t work_h = height;
    double scale_x = 1.0;
    double scale_y = 1.0;
    double eff_dilation = (double)s_cfg.dilation_px;
    if (width > WE_CDT_MAX_WORK_DIM || height > WE_CDT_MAX_WORK_DIM) {
        double f = (double)WE_CDT_MAX_WORK_DIM / (double)(width > height ? width : height);
        work_w = (uint32_t)lround((double)width * f);
        work_h = (uint32_t)lround((double)height * f);
        if (work_w < 1u) {
            work_w = 1u;
        }
        if (work_h < 1u) {
            work_h = 1u;
        }
        scale_x = (double)width / (double)work_w;
        scale_y = (double)height / (double)work_h;
        eff_dilation = (double)s_cfg.dilation_px / fmax(scale_x, scale_y);
    }

    uint32_t radius = (uint32_t)ceil(eff_dilation);
    if (radius > 32u) {
        radius = 32u;
    }
    uint32_t pad_border = radius + 1u;
    uint32_t wp = work_w + 2u * pad_border;
    uint32_t hp = work_h + 2u * pad_border;
    if ((uint64_t)wp > SIZE_MAX / (uint64_t)hp / 2u) {
        return false;
    }
    size_t pad_pixels = (size_t)wp * (size_t)hp;

    uint8_t *pad_a = calloc(pad_pixels, 1u);
    if (!pad_a) {
        return false;
    }
    if (work_w == width && work_h == height) {
        for (uint32_t y = 0u; y < height; y++) {
            memcpy(pad_a + (size_t)(y + pad_border) * wp + pad_border,
                   alpha + (size_t)y * stride_bytes, width);
        }
    } else {
        for (uint32_t wy = 0u; wy < work_h; wy++) {
            uint32_t y0 = (uint32_t)(((uint64_t)wy * (uint64_t)height) / work_h);
            uint32_t y1 =
                (uint32_t)(((uint64_t)(wy + 1u) * (uint64_t)height + work_h - 1u) / work_h);
            if (y1 > height) {
                y1 = height;
            }
            uint8_t *dst_row = pad_a + (size_t)(wy + pad_border) * wp + pad_border;
            for (uint32_t wx = 0u; wx < work_w; wx++) {
                uint32_t x0 = (uint32_t)(((uint64_t)wx * (uint64_t)width) / work_w);
                uint32_t x1 =
                    (uint32_t)(((uint64_t)(wx + 1u) * (uint64_t)width + work_w - 1u) / work_w);
                if (x1 > width) {
                    x1 = width;
                }
                uint8_t max_a = 0u;
                for (uint32_t sy = y0; sy < y1; sy++) {
                    const uint8_t *src_row = alpha + (size_t)sy * stride_bytes;
                    for (uint32_t sx = x0; sx < x1; sx++) {
                        if (src_row[sx] > max_a) {
                            max_a = src_row[sx];
                        }
                    }
                }
                dst_row[wx] = max_a;
            }
        }
    }

    if (radius > 0u) {
        uint8_t *pad_b = calloc(pad_pixels, 1u);
        uint32_t *dq_buf = malloc((size_t)(2u * radius + 4u) * sizeof(uint32_t));
        if (!pad_b || !dq_buf) {
            free(dq_buf);
            free(pad_b);
            free(pad_a);
            return false;
        }
        for (uint32_t j = pad_border; j < hp - pad_border; j++) {
            dilate_1d_max(pad_a + (size_t)j * wp, pad_b + (size_t)j * wp, wp, 1, radius, dq_buf);
        }
        for (uint32_t i = 1u; i < wp - 1u; i++) {
            dilate_1d_max(pad_b + i, pad_a + i, hp, (ptrdiff_t)wp, radius, dq_buf);
        }
        free(dq_buf);
        free(pad_b);
    }

    int32_t tau8 = (int32_t)lroundf(s_cfg.alpha_threshold * 255.0f);
    if (tau8 < 1) {
        tau8 = 1;
    } else if (tau8 > 255) {
        tau8 = 255;
    }
    int32_t bias = 2 * tau8 - 1;

    size_t bitset_bytes = (pad_pixels * 2u + 7u) / 8u;
    uint8_t *visited_bits = calloc(bitset_bytes, 1u);
    if (!visited_bits) {
        free(pad_a);
        return false;
    }

    we_cdt_loop_t *loops = NULL;
    uint32_t loop_count = 0u;
    uint32_t loop_cap = 0u;

    size_t pts_cap = 256u;
    we_cdt_vec2_t *pts_buf = malloc(pts_cap * sizeof(we_cdt_vec2_t));
    if (!pts_buf) {
        free(visited_bits);
        free(pad_a);
        return false;
    }

    bool ok = true;
    for (uint32_t j = 0u; j + 1u < hp && ok; j++) {
        for (uint32_t i = 0u; i + 1u < wp && ok; i++) {
            int32_t s00 = 2 * (int32_t)pad_a[(size_t)j * wp + i] - bias;
            int32_t s10 = 2 * (int32_t)pad_a[(size_t)j * wp + i + 1u] - bias;
            int32_t s11 = 2 * (int32_t)pad_a[(size_t)(j + 1u) * wp + i + 1u] - bias;
            int32_t s01 = 2 * (int32_t)pad_a[(size_t)(j + 1u) * wp + i] - bias;
            uint8_t mask = (uint8_t)(((s00 > 0) ? 1u : 0u) | ((s10 > 0) ? 2u : 0u) |
                                     ((s11 > 0) ? 4u : 0u) | ((s01 > 0) ? 8u : 0u));
            if (mask == 0u || mask == 15u) {
                continue;
            }

            bool enter_flags[4] = {
                (s00 > 0 && s10 < 0),
                (s10 > 0 && s11 < 0),
                (s11 > 0 && s01 < 0),
                (s01 > 0 && s00 < 0),
            };

            for (uint32_t side_u = 0u; side_u < 4u && ok; side_u++) {
                if (!enter_flags[side_u]) {
                    continue;
                }
                ms_side_t start_side = (ms_side_t)side_u;
                size_t start_eid = ms_edge_id(i, j, start_side, wp);
                if (bitset_test_and_set(visited_bits, start_eid)) {
                    continue;
                }

                uint32_t ci = i;
                uint32_t cj = j;
                ms_side_t cur_side = start_side;
                uint32_t pts_len = 0u;
                size_t max_steps = pad_pixels * 2u + 8u;
                size_t steps = 0u;

                for (;;) {
                    if (steps++ > max_steps) {
                        ok = false;
                        break;
                    }
                    int32_t c00 = 2 * (int32_t)pad_a[(size_t)cj * wp + ci] - bias;
                    int32_t c10 = 2 * (int32_t)pad_a[(size_t)cj * wp + ci + 1u] - bias;
                    int32_t c11 = 2 * (int32_t)pad_a[(size_t)(cj + 1u) * wp + ci + 1u] - bias;
                    int32_t c01 = 2 * (int32_t)pad_a[(size_t)(cj + 1u) * wp + ci] - bias;
                    uint8_t cmask = (uint8_t)(((c00 > 0) ? 1u : 0u) | ((c10 > 0) ? 2u : 0u) |
                                              ((c11 > 0) ? 4u : 0u) | ((c01 > 0) ? 8u : 0u));

                    we_cdt_vec2_t pt =
                        ms_crossing_pos(ci, cj, cur_side, pad_border, c00, c10, c11, c01);
                    pt.x *= scale_x;
                    pt.y *= scale_y;
                    if (pts_len == 0u || dist2_pts(pts_buf[pts_len - 1u], pt) > 1e-20) {
                        if (pts_len == pts_cap) {
                            size_t next_cap = pts_cap * 2u;
                            we_cdt_vec2_t *nb = realloc(pts_buf, next_cap * sizeof(we_cdt_vec2_t));
                            if (!nb) {
                                ok = false;
                                break;
                            }
                            pts_buf = nb;
                            pts_cap = next_cap;
                        }
                        pts_buf[pts_len++] = pt;
                    }

                    ms_side_t exit_side = ms_cell_exit_side(c00, c10, c11, c01, cmask, cur_side);
                    uint32_t ni = ci;
                    uint32_t nj = cj;
                    ms_side_t next_enter;
                    switch (exit_side) {
                    case MS_SIDE_TOP:
                        if (cj == 0u) {
                            ok = false;
                            break;
                        }
                        nj = cj - 1u;
                        next_enter = MS_SIDE_BOTTOM;
                        break;
                    case MS_SIDE_RIGHT:
                        if (ci + 2u >= wp) {
                            ok = false;
                            break;
                        }
                        ni = ci + 1u;
                        next_enter = MS_SIDE_LEFT;
                        break;
                    case MS_SIDE_BOTTOM:
                        if (cj + 2u >= hp) {
                            ok = false;
                            break;
                        }
                        nj = cj + 1u;
                        next_enter = MS_SIDE_TOP;
                        break;
                    case MS_SIDE_LEFT:
                    default:
                        if (ci == 0u) {
                            ok = false;
                            break;
                        }
                        ni = ci - 1u;
                        next_enter = MS_SIDE_RIGHT;
                        break;
                    }
                    if (!ok) {
                        break;
                    }
                    if (ni == i && nj == j && next_enter == start_side) {
                        break;
                    }
                    size_t next_eid = ms_edge_id(ni, nj, next_enter, wp);
                    if (bitset_test_and_set(visited_bits, next_eid)) {
                        ok = false;
                        break;
                    }
                    ci = ni;
                    cj = nj;
                    cur_side = next_enter;
                }

                if (!ok) {
                    break;
                }
                if (pts_len > 2u && dist2_pts(pts_buf[pts_len - 1u], pts_buf[0]) <= 1e-20) {
                    pts_len--;
                }
                if (pts_len < 3u) {
                    continue;
                }

                double area = compute_signed_area(pts_buf, pts_len);
                if (fabs(area) < (double)s_cfg.min_island_area || fabs(area) <= 1e-12) {
                    continue;
                }

                if (loop_count == loop_cap) {
                    uint32_t next_lcap = (loop_cap == 0u) ? 8u : (loop_cap * 2u);
                    we_cdt_loop_t *nl = realloc(loops, (size_t)next_lcap * sizeof(we_cdt_loop_t));
                    if (!nl) {
                        ok = false;
                        break;
                    }
                    loops = nl;
                    loop_cap = next_lcap;
                }

                we_cdt_vec2_t *copy = malloc((size_t)pts_len * sizeof(we_cdt_vec2_t));
                if (!copy) {
                    ok = false;
                    break;
                }
                memcpy(copy, pts_buf, (size_t)pts_len * sizeof(we_cdt_vec2_t));
                loops[loop_count].points = copy;
                loops[loop_count].point_count = pts_len;
                loops[loop_count].is_hole = (area < 0.0);
                loops[loop_count].signed_area_px2 = area;
                loop_count++;
            }
        }
    }

    free(pts_buf);
    free(visited_bits);
    free(pad_a);

    if (!ok) {
        we_mesh_loops_free(loops, loop_count);
        return false;
    }

    *out_loops = loops;
    *out_loop_count = loop_count;
    return true;
}

void we_mesh_loops_free(we_cdt_loop_t *loops, uint32_t loop_count)
{
    if (!loops) {
        return;
    }
    for (uint32_t i = 0u; i < loop_count; i++) {
        free(loops[i].points);
    }
    free(loops);
}

/* =========================================================================
 * Step 2: Saalfeld (1999) Topology-Preserving Ramer-Douglas-Peucker
 * ========================================================================= */

typedef struct {
    double x;
    double y;
    uint32_t loop_id;
    uint32_t next_v;
    uint32_t prev_v;
    bool active;
    bool seg_active;
} rdp_vert_t;

typedef struct {
    uint32_t id;
    int32_t next;
} rdp_grid_node_t;

typedef struct {
    uint32_t gx;
    uint32_t gy;
    double min_x;
    double min_y;
    double inv_cell_w;
    double inv_cell_h;
    int32_t *seg_heads;
    int32_t *vert_heads;
    rdp_grid_node_t *pool;
    uint32_t pool_len;
    uint32_t pool_cap;
    uint32_t *stamp;
    uint32_t cur_stamp;
} rdp_spatial_grid_t;

static void rdp_grid_cell_range(const rdp_spatial_grid_t *g, double minx, double miny, double maxx,
                                double maxy, uint32_t *ix0, uint32_t *iy0, uint32_t *ix1,
                                uint32_t *iy1)
{
    int64_t x0 = (int64_t)floor((minx - g->min_x) * g->inv_cell_w);
    int64_t y0 = (int64_t)floor((miny - g->min_y) * g->inv_cell_h);
    int64_t x1 = (int64_t)floor((maxx - g->min_x) * g->inv_cell_w);
    int64_t y1 = (int64_t)floor((maxy - g->min_y) * g->inv_cell_h);
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 >= (int64_t)g->gx) {
        x1 = (int64_t)g->gx - 1;
    }
    if (y1 >= (int64_t)g->gy) {
        y1 = (int64_t)g->gy - 1;
    }
    if (x1 < x0) {
        x1 = x0;
    }
    if (y1 < y0) {
        y1 = y0;
    }
    *ix0 = (uint32_t)x0;
    *iy0 = (uint32_t)y0;
    *ix1 = (uint32_t)x1;
    *iy1 = (uint32_t)y1;
}

static bool rdp_grid_push_node(rdp_spatial_grid_t *g, int32_t *head_slot, uint32_t id)
{
    if (g->pool_len == g->pool_cap) {
        uint32_t next_cap = (g->pool_cap == 0u) ? 1024u : (g->pool_cap * 2u);
        rdp_grid_node_t *np = realloc(g->pool, (size_t)next_cap * sizeof(rdp_grid_node_t));
        if (!np) {
            return false;
        }
        g->pool = np;
        g->pool_cap = next_cap;
    }
    int32_t idx = (int32_t)g->pool_len++;
    g->pool[idx].id = id;
    g->pool[idx].next = *head_slot;
    *head_slot = idx;
    return true;
}

static bool rdp_grid_insert_seg(rdp_spatial_grid_t *g, uint32_t seg_id, we_cdt_vec2_t a,
                                we_cdt_vec2_t b)
{
    uint32_t ix0, iy0, ix1, iy1;
    rdp_grid_cell_range(g, fmin(a.x, b.x), fmin(a.y, b.y), fmax(a.x, b.x), fmax(a.y, b.y), &ix0,
                        &iy0, &ix1, &iy1);
    for (uint32_t cy = iy0; cy <= iy1; cy++) {
        for (uint32_t cx = ix0; cx <= ix1; cx++) {
            if (!rdp_grid_push_node(g, &g->seg_heads[cy * g->gx + cx], seg_id)) {
                return false;
            }
        }
    }
    return true;
}

static bool rdp_grid_insert_vert(rdp_spatial_grid_t *g, uint32_t vert_id, we_cdt_vec2_t p)
{
    uint32_t ix0, iy0, ix1, iy1;
    rdp_grid_cell_range(g, p.x, p.y, p.x, p.y, &ix0, &iy0, &ix1, &iy1);
    return rdp_grid_push_node(g, &g->vert_heads[iy0 * g->gx + ix0], vert_id);
}

static void rdp_grid_free(rdp_spatial_grid_t *g)
{
    free(g->seg_heads);
    free(g->vert_heads);
    free(g->pool);
    free(g->stamp);
    memset(g, 0, sizeof(*g));
}

static double perp_dist_point_chord(we_cdt_vec2_t p, we_cdt_vec2_t a, we_cdt_vec2_t b)
{
    double vx = b.x - a.x;
    double vy = b.y - a.y;
    double len = hypot(vx, vy);
    if (len <= 1e-12) {
        return hypot(p.x - a.x, p.y - a.y);
    }
    double cross = fabs(vx * (p.y - a.y) - vy * (p.x - a.x));
    return cross / len;
}

typedef struct {
    uint32_t lo;
    uint32_t hi;
} rdp_span_t;

static bool tp_rdp_simplify_loops(const we_cdt_loop_t *in_loops, uint32_t in_count, double eps_px,
                                  we_cdt_loop_t **out_loops, uint32_t *out_count)
{
    *out_loops = NULL;
    *out_count = 0u;

    uint32_t total_verts = 0u;
    uint32_t valid_loops = 0u;
    for (uint32_t m = 0u; m < in_count; m++) {
        if (in_loops[m].points && in_loops[m].point_count >= 3u) {
            if (total_verts > UINT32_MAX - in_loops[m].point_count) {
                return false;
            }
            total_verts += in_loops[m].point_count;
            valid_loops++;
        }
    }
    if (valid_loops == 0u || total_verts < 3u) {
        return false;
    }

    rdp_vert_t *verts = calloc(total_verts, sizeof(rdp_vert_t));
    uint32_t *loop_offsets = calloc(valid_loops, sizeof(uint32_t));
    uint32_t *loop_orig_cnt = calloc(valid_loops, sizeof(uint32_t));
    uint32_t *loop_active_cnt = calloc(valid_loops, sizeof(uint32_t));
    double *loop_2area = calloc(valid_loops, sizeof(double));
    bool *loop_is_hole = calloc(valid_loops, sizeof(bool));
    uint32_t *chain_buf = malloc((size_t)total_verts * sizeof(uint32_t));
    we_cdt_vec2_t *poly_buf = malloc((size_t)(total_verts + 1u) * sizeof(we_cdt_vec2_t));

    if (!verts || !loop_offsets || !loop_orig_cnt || !loop_active_cnt || !loop_2area ||
        !loop_is_hole || !chain_buf || !poly_buf) {
        free(poly_buf);
        free(chain_buf);
        free(loop_is_hole);
        free(loop_2area);
        free(loop_active_cnt);
        free(loop_orig_cnt);
        free(loop_offsets);
        free(verts);
        return false;
    }

    double min_x = DBL_MAX;
    double min_y = DBL_MAX;
    double max_x = -DBL_MAX;
    double max_y = -DBL_MAX;

    uint32_t cur_v = 0u;
    uint32_t cur_l = 0u;
    for (uint32_t m = 0u; m < in_count; m++) {
        if (!in_loops[m].points || in_loops[m].point_count < 3u) {
            continue;
        }
        uint32_t start = cur_v;
        uint32_t raw_n = in_loops[m].point_count;
        for (uint32_t k = 0u; k < raw_n; k++) {
            we_cdt_vec2_t p = in_loops[m].points[k];
            if (!isfinite(p.x) || !isfinite(p.y) || fabs(p.x) > 1e9 || fabs(p.y) > 1e9) {
                free(poly_buf);
                free(chain_buf);
                free(loop_is_hole);
                free(loop_2area);
                free(loop_active_cnt);
                free(loop_orig_cnt);
                free(loop_offsets);
                free(verts);
                return false;
            }
            if (cur_v > start &&
                dist2_pts(p, (we_cdt_vec2_t){verts[cur_v - 1u].x, verts[cur_v - 1u].y}) <= 1e-18) {
                continue;
            }
            verts[cur_v].x = p.x;
            verts[cur_v].y = p.y;
            verts[cur_v].loop_id = cur_l;
            verts[cur_v].active = true;
            verts[cur_v].seg_active = true;
            cur_v++;
        }
        if (cur_v - start > 1u &&
            dist2_pts((we_cdt_vec2_t){verts[cur_v - 1u].x, verts[cur_v - 1u].y},
                      (we_cdt_vec2_t){verts[start].x, verts[start].y}) <= 1e-18) {
            cur_v--;
        }
        uint32_t cnt = cur_v - start;
        if (cnt < 3u) {
            cur_v = start;
            continue;
        }

        for (uint32_t k = 0u; k < cnt; k++) {
            poly_buf[k] = (we_cdt_vec2_t){verts[start + k].x, verts[start + k].y};
        }
        double a2 = 2.0 * compute_signed_area(poly_buf, cnt);
        if (fabs(a2) <= 1e-12) {
            cur_v = start;
            continue;
        }

        bool want_hole = in_loops[m].is_hole;
        if ((!want_hole && a2 < 0.0) || (want_hole && a2 > 0.0)) {
            for (uint32_t k = 0u; k < cnt / 2u; k++) {
                rdp_vert_t tmp = verts[start + k];
                verts[start + k] = verts[start + cnt - 1u - k];
                verts[start + cnt - 1u - k] = tmp;
            }
            a2 = -a2;
        }

        for (uint32_t k = 0u; k < cnt; k++) {
            uint32_t idx = start + k;
            verts[idx].next_v = start + ((k + 1u < cnt) ? (k + 1u) : 0u);
            verts[idx].prev_v = start + ((k > 0u) ? (k - 1u) : (cnt - 1u));
            min_x = fmin(min_x, verts[idx].x);
            min_y = fmin(min_y, verts[idx].y);
            max_x = fmax(max_x, verts[idx].x);
            max_y = fmax(max_y, verts[idx].y);
        }

        loop_offsets[cur_l] = start;
        loop_orig_cnt[cur_l] = cnt;
        loop_active_cnt[cur_l] = cnt;
        loop_2area[cur_l] = a2;
        loop_is_hole[cur_l] = want_hole;
        cur_l++;
    }

    uint32_t active_loops = cur_l;
    uint32_t used_verts = cur_v;
    if (active_loops == 0u || used_verts < 3u) {
        free(poly_buf);
        free(chain_buf);
        free(loop_is_hole);
        free(loop_2area);
        free(loop_active_cnt);
        free(loop_orig_cnt);
        free(loop_offsets);
        free(verts);
        return false;
    }

    uint32_t gdim = (uint32_t)sqrt((double)used_verts);
    if (gdim < 16u) {
        gdim = 16u;
    } else if (gdim > 96u) {
        gdim = 96u;
    }
    double span_x = fmax(max_x - min_x, 1.0);
    double span_y = fmax(max_y - min_y, 1.0);

    rdp_spatial_grid_t grid;
    memset(&grid, 0, sizeof(grid));
    grid.gx = gdim;
    grid.gy = gdim;
    grid.min_x = min_x - 1e-6;
    grid.min_y = min_y - 1e-6;
    grid.inv_cell_w = (double)gdim / (span_x + 2e-6);
    grid.inv_cell_h = (double)gdim / (span_y + 2e-6);
    grid.seg_heads = malloc((size_t)gdim * gdim * sizeof(int32_t));
    grid.vert_heads = malloc((size_t)gdim * gdim * sizeof(int32_t));
    grid.stamp = calloc(used_verts, sizeof(uint32_t));
    if (!grid.seg_heads || !grid.vert_heads || !grid.stamp) {
        rdp_grid_free(&grid);
        free(poly_buf);
        free(chain_buf);
        free(loop_is_hole);
        free(loop_2area);
        free(loop_active_cnt);
        free(loop_orig_cnt);
        free(loop_offsets);
        free(verts);
        return false;
    }
    for (size_t i = 0u; i < (size_t)gdim * gdim; i++) {
        grid.seg_heads[i] = -1;
        grid.vert_heads[i] = -1;
    }

    bool grid_ok = true;
    for (uint32_t i = 0u; i < used_verts && grid_ok; i++) {
        we_cdt_vec2_t a = {verts[i].x, verts[i].y};
        we_cdt_vec2_t b = {verts[verts[i].next_v].x, verts[verts[i].next_v].y};
        grid_ok = rdp_grid_insert_vert(&grid, i, a) && rdp_grid_insert_seg(&grid, i, a, b);
    }

    uint32_t stack_cap = 256u;
    rdp_span_t *stack = malloc(stack_cap * sizeof(rdp_span_t));
    if (!grid_ok || !stack) {
        free(stack);
        rdp_grid_free(&grid);
        free(poly_buf);
        free(chain_buf);
        free(loop_is_hole);
        free(loop_2area);
        free(loop_active_cnt);
        free(loop_orig_cnt);
        free(loop_offsets);
        free(verts);
        return false;
    }

    bool rdp_ok = true;
    for (uint32_t m = 0u; m < active_loops && rdp_ok; m++) {
        uint32_t off = loop_offsets[m];
        uint32_t cnt = loop_orig_cnt[m];
        if (cnt <= 3u) {
            continue;
        }

        uint32_t i_min = 0u;
        for (uint32_t k = 1u; k < cnt; k++) {
            if (verts[off + k].x < verts[off + i_min].x ||
                (verts[off + k].x == verts[off + i_min].x &&
                 verts[off + k].y < verts[off + i_min].y)) {
                i_min = k;
            }
        }
        uint32_t i_max = 0u;
        double best_d2 = -1.0;
        we_cdt_vec2_t p_min = {verts[off + i_min].x, verts[off + i_min].y};
        for (uint32_t k = 0u; k < cnt; k++) {
            if (k == i_min) {
                continue;
            }
            double d2 = dist2_pts(p_min, (we_cdt_vec2_t){verts[off + k].x, verts[off + k].y});
            if (d2 > best_d2) {
                best_d2 = d2;
                i_max = k;
            }
        }
        if (i_min == i_max) {
            continue;
        }

        for (int half = 0; half < 2 && rdp_ok; half++) {
            uint32_t s_local = (half == 0) ? i_min : i_max;
            uint32_t e_local = (half == 0) ? i_max : i_min;
            uint32_t len = 0u;
            uint32_t cur = s_local;
            for (;;) {
                chain_buf[len++] = off + cur;
                if (cur == e_local) {
                    break;
                }
                cur = (cur + 1u < cnt) ? (cur + 1u) : 0u;
            }
            if (len <= 2u) {
                continue;
            }

            uint32_t sp = 0u;
            stack[sp++] = (rdp_span_t){0u, len - 1u};

            while (sp > 0u && rdp_ok) {
                rdp_span_t span = stack[--sp];
                if (span.hi - span.lo <= 1u) {
                    continue;
                }

                uint32_t va = chain_buf[span.lo];
                uint32_t vb = chain_buf[span.hi];
                if (!verts[va].active || !verts[vb].active) {
                    continue;
                }
                we_cdt_vec2_t pa = {verts[va].x, verts[va].y};
                we_cdt_vec2_t pb = {verts[vb].x, verts[vb].y};

                double max_d = -1.0;
                uint32_t k_max = span.lo;
                for (uint32_t k = span.lo + 1u; k < span.hi; k++) {
                    uint32_t vk = chain_buf[k];
                    if (!verts[vk].active) {
                        continue;
                    }
                    double d =
                        perp_dist_point_chord((we_cdt_vec2_t){verts[vk].x, verts[vk].y}, pa, pb);
                    if (d > max_d) {
                        max_d = d;
                        k_max = k;
                    }
                }

                bool can_collapse = (max_d <= eps_px);
                uint32_t q_len = 0u;
                double new_2area = loop_2area[m];
                if (can_collapse) {
                    for (uint32_t k = span.lo; k <= span.hi; k++) {
                        uint32_t vk = chain_buf[k];
                        if (verts[vk].active) {
                            poly_buf[q_len++] = (we_cdt_vec2_t){verts[vk].x, verts[vk].y};
                        }
                    }
                    if (q_len <= 2u) {
                        continue;
                    }
                    uint32_t dropped = q_len - 2u;
                    if (loop_active_cnt[m] < 3u + dropped) {
                        can_collapse = false;
                    }
                }

                if (can_collapse) {
                    double ox = pa.x;
                    double oy = pa.y;
                    double old_sum = 0.0;
                    for (uint32_t t = 0u; t + 1u < q_len; t++) {
                        double x0 = poly_buf[t].x - ox;
                        double y0 = poly_buf[t].y - oy;
                        double x1 = poly_buf[t + 1u].x - ox;
                        double y1 = poly_buf[t + 1u].y - oy;
                        old_sum += x0 * y1 - x1 * y0;
                    }
                    /* In the pa-shifted frame the replacement chord pa->pb contributes zero area.
                     */
                    new_2area = loop_2area[m] - old_sum;
                    if (loop_2area[m] > 0.0) {
                        if (new_2area <= 1e-9) {
                            can_collapse = false;
                        }
                    } else {
                        if (new_2area >= -1e-9) {
                            can_collapse = false;
                        }
                    }
                }

                if (can_collapse) {
                    /* Mark subchain vertices in stamp to avoid false non-incident hits */
                    uint32_t s_mark = ++grid.cur_stamp;
                    if (s_mark == 0u) {
                        memset(grid.stamp, 0, used_verts * sizeof(uint32_t));
                        s_mark = ++grid.cur_stamp;
                    }
                    /* Temporarily record chain vertex membership via verts[].seg_active? No,
                       we only check segment intersection against active segments outside [va..vb),
                       plus foldback on prev(va)->va and vb->next(vb). */
                    uint32_t prev_va = verts[va].prev_v;
                    uint32_t next_vb = verts[vb].next_v;
                    we_cdt_vec2_t p_prev = {verts[prev_va].x, verts[prev_va].y};
                    we_cdt_vec2_t p_next = {verts[next_vb].x, verts[next_vb].y};

                    if (orient2d(pa, pb, p_prev) == 0.0 &&
                        ((p_prev.x - pa.x) * (pb.x - pa.x) + (p_prev.y - pa.y) * (pb.y - pa.y)) >
                            0.0) {
                        can_collapse = false;
                    }
                    if (can_collapse && orient2d(pa, pb, p_next) == 0.0 &&
                        ((p_next.x - pb.x) * (pa.x - pb.x) + (p_next.y - pb.y) * (pa.y - pb.y)) >
                            0.0) {
                        can_collapse = false;
                    }

                    if (can_collapse) {
                        for (uint32_t k = span.lo; k < span.hi; k++) {
                            grid.stamp[chain_buf[k]] = s_mark;
                        }
                        grid.stamp[prev_va] = s_mark;
                        grid.stamp[vb] = s_mark;

                        uint32_t ix0, iy0, ix1, iy1;
                        rdp_grid_cell_range(&grid, fmin(pa.x, pb.x), fmin(pa.y, pb.y),
                                            fmax(pa.x, pb.x), fmax(pa.y, pb.y), &ix0, &iy0, &ix1,
                                            &iy1);
                        for (uint32_t cy = iy0; cy <= iy1 && can_collapse; cy++) {
                            for (uint32_t cx = ix0; cx <= ix1 && can_collapse; cx++) {
                                for (int32_t ni = grid.seg_heads[cy * grid.gx + cx]; ni >= 0;
                                     ni = grid.pool[ni].next) {
                                    uint32_t u = grid.pool[ni].id;
                                    if (!verts[u].active || !verts[u].seg_active) {
                                        continue;
                                    }
                                    if (grid.stamp[u] == s_mark) {
                                        continue;
                                    }
                                    grid.stamp[u] = s_mark;
                                    we_cdt_vec2_t su = {verts[u].x, verts[u].y};
                                    uint32_t w = verts[u].next_v;
                                    we_cdt_vec2_t sw = {verts[w].x, verts[w].y};
                                    if (segments_intersect_or_touch(pa, pb, su, sw)) {
                                        can_collapse = false;
                                        break;
                                    }
                                }
                            }
                        }
                    }

                    if (can_collapse) {
                        double bminx = poly_buf[0].x;
                        double bminy = poly_buf[0].y;
                        double bmaxx = poly_buf[0].x;
                        double bmaxy = poly_buf[0].y;
                        for (uint32_t t = 1u; t < q_len; t++) {
                            bminx = fmin(bminx, poly_buf[t].x);
                            bminy = fmin(bminy, poly_buf[t].y);
                            bmaxx = fmax(bmaxx, poly_buf[t].x);
                            bmaxy = fmax(bmaxy, poly_buf[t].y);
                        }
                        uint32_t v_mark = ++grid.cur_stamp;
                        if (v_mark == 0u) {
                            memset(grid.stamp, 0, used_verts * sizeof(uint32_t));
                            v_mark = ++grid.cur_stamp;
                        }
                        for (uint32_t k = span.lo; k <= span.hi; k++) {
                            grid.stamp[chain_buf[k]] = v_mark;
                        }

                        uint32_t ix0, iy0, ix1, iy1;
                        rdp_grid_cell_range(&grid, bminx, bminy, bmaxx, bmaxy, &ix0, &iy0, &ix1,
                                            &iy1);
                        for (uint32_t cy = iy0; cy <= iy1 && can_collapse; cy++) {
                            for (uint32_t cx = ix0; cx <= ix1 && can_collapse; cx++) {
                                for (int32_t ni = grid.vert_heads[cy * grid.gx + cx]; ni >= 0;
                                     ni = grid.pool[ni].next) {
                                    uint32_t vid = grid.pool[ni].id;
                                    if (!verts[vid].active || grid.stamp[vid] == v_mark) {
                                        continue;
                                    }
                                    grid.stamp[vid] = v_mark;
                                    we_cdt_vec2_t vp = {verts[vid].x, verts[vid].y};
                                    if (vp.x < bminx || vp.x > bmaxx || vp.y < bminy ||
                                        vp.y > bmaxy) {
                                        continue;
                                    }
                                    if (point_in_or_on_polygon(vp, poly_buf, q_len)) {
                                        can_collapse = false;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }

                if (can_collapse) {
                    uint32_t dropped = 0u;
                    for (uint32_t k = span.lo + 1u; k < span.hi; k++) {
                        uint32_t vk = chain_buf[k];
                        if (verts[vk].active) {
                            verts[vk].active = false;
                            verts[vk].seg_active = false;
                            dropped++;
                        }
                    }
                    verts[va].next_v = vb;
                    verts[vb].prev_v = va;
                    verts[va].seg_active = true;
                    loop_active_cnt[m] -= dropped;
                    loop_2area[m] = new_2area;
                    if (!rdp_grid_insert_seg(&grid, va, pa, pb)) {
                        rdp_ok = false;
                    }
                } else {
                    uint32_t k_split = k_max;
                    if (k_split <= span.lo || k_split >= span.hi) {
                        k_split = span.lo + (span.hi - span.lo) / 2u;
                    }
                    if (sp + 2u > stack_cap) {
                        uint32_t next_scap = stack_cap * 2u;
                        rdp_span_t *ns = realloc(stack, next_scap * sizeof(rdp_span_t));
                        if (!ns) {
                            rdp_ok = false;
                            break;
                        }
                        stack = ns;
                        stack_cap = next_scap;
                    }
                    stack[sp++] = (rdp_span_t){k_split, span.hi};
                    stack[sp++] = (rdp_span_t){span.lo, k_split};
                }
            }
        }
    }

    free(stack);
    rdp_grid_free(&grid);

    we_cdt_loop_t *res = NULL;
    if (rdp_ok) {
        res = calloc(active_loops, sizeof(we_cdt_loop_t));
        if (!res) {
            rdp_ok = false;
        }
    }
    uint32_t out_n = 0u;
    for (uint32_t m = 0u; m < active_loops && rdp_ok; m++) {
        uint32_t start = loop_offsets[m];
        uint32_t n_act = loop_active_cnt[m];
        we_cdt_vec2_t *pts = malloc((size_t)n_act * sizeof(we_cdt_vec2_t));
        if (!pts) {
            rdp_ok = false;
            break;
        }
        uint32_t cur = start;
        while (!verts[cur].active) {
            cur = (cur + 1u);
        }
        uint32_t written = 0u;
        uint32_t walk = cur;
        for (uint32_t step = 0u; step < n_act; step++) {
            pts[written++] = (we_cdt_vec2_t){verts[walk].x, verts[walk].y};
            walk = verts[walk].next_v;
        }
        res[out_n].points = pts;
        res[out_n].point_count = written;
        res[out_n].is_hole = loop_is_hole[m];
        res[out_n].signed_area_px2 = 0.5 * loop_2area[m];
        out_n++;
    }

    free(poly_buf);
    free(chain_buf);
    free(loop_is_hole);
    free(loop_2area);
    free(loop_active_cnt);
    free(loop_orig_cnt);
    free(loop_offsets);
    free(verts);

    if (!rdp_ok) {
        we_mesh_loops_free(res, out_n);
        return false;
    }

    *out_loops = res;
    *out_count = out_n;
    return true;
}

/* =========================================================================
 * Step 3: Constrained Delaunay Triangulation, Bridson & Chew/Ruppert
 * ========================================================================= */

#define TRI_ALIVE    (1u << 0u)
#define TRI_INTERIOR (1u << 1u)

typedef struct {
    double x;
    double y;
    int32_t any_tri;
    bool is_super;
    bool is_boundary;
    bool is_pin;
    bool has_acute_corner;
} cdt_vert_t;

typedef struct {
    int32_t v[3];
    int32_t adj[3];
    uint8_t constrained; /* bit e => edge (v[e], v[(e+1)%3]) is constrained */
    uint8_t flags;
    int32_t heap_idx;
    double b2;
} cdt_tri_t;

typedef struct {
    int32_t tri;
    int32_t edge;
} cdt_edge_ref_t;

typedef struct {
    int32_t va;
    int32_t vb;
} cdt_seg_pair_t;

typedef struct {
    cdt_vert_t *verts;
    uint32_t v_count;
    uint32_t v_cap;

    cdt_tri_t *tris;
    uint32_t t_count;
    uint32_t t_cap;
    int32_t *free_tris;
    uint32_t free_count;

    int32_t *heap;
    uint32_t heap_len;
    uint32_t heap_cap;

    cdt_edge_ref_t *flip_stack;
    uint32_t flip_cap;

    int32_t *tri_visit_stamp;
    int32_t cur_stamp;

    double b_max2;
    double cos_theta_min;
    double min_edge_len2;
    double dup_dist2;
    uint32_t max_non_super_verts;
    bool hit_vertex_budget;
} cdt_mesh_t;

static void cdt_mesh_free(cdt_mesh_t *m)
{
    free(m->verts);
    free(m->tris);
    free(m->free_tris);
    free(m->heap);
    free(m->flip_stack);
    free(m->tri_visit_stamp);
    memset(m, 0, sizeof(*m));
}

static bool cdt_mesh_init(cdt_mesh_t *m, uint32_t max_verts, double min_angle_deg)
{
    memset(m, 0, sizeof(*m));
    m->v_cap = max_verts + 3u;
    m->t_cap = m->v_cap * 2u + 64u;
    if (m->t_cap < WE_CDT_INTERNAL_TRI_CAP) {
        m->t_cap = WE_CDT_INTERNAL_TRI_CAP;
    }
    m->heap_cap = m->t_cap;
    m->flip_cap = 1024u;

    m->verts = calloc(m->v_cap, sizeof(cdt_vert_t));
    m->tris = calloc(m->t_cap, sizeof(cdt_tri_t));
    m->free_tris = malloc(m->t_cap * sizeof(int32_t));
    m->heap = malloc(m->heap_cap * sizeof(int32_t));
    m->flip_stack = malloc(m->flip_cap * sizeof(cdt_edge_ref_t));
    m->tri_visit_stamp = calloc(m->t_cap, sizeof(int32_t));

    if (!m->verts || !m->tris || !m->free_tris || !m->heap || !m->flip_stack ||
        !m->tri_visit_stamp) {
        cdt_mesh_free(m);
        return false;
    }

    double rad = min_angle_deg * (M_PI / 180.0);
    double s = sin(rad);
    m->b_max2 = 1.0 / (4.0 * s * s);
    m->cos_theta_min = cos(rad);
    m->min_edge_len2 = 1e-6;
    m->dup_dist2 = 1e-6;
    m->max_non_super_verts = max_verts;
    return true;
}

static inline we_cdt_vec2_t cdt_vp(const cdt_mesh_t *m, int32_t vid)
{
    return (we_cdt_vec2_t){m->verts[vid].x, m->verts[vid].y};
}

static int32_t cdt_alloc_tri(cdt_mesh_t *m)
{
    int32_t tid;
    if (m->free_count > 0u) {
        tid = m->free_tris[--m->free_count];
    } else {
        if (m->t_count == m->t_cap) {
            if (m->t_cap > UINT32_MAX / 2u ||
                (size_t)m->t_cap * 2u > SIZE_MAX / sizeof(cdt_tri_t)) {
                return -1;
            }
            uint32_t next_cap = m->t_cap * 2u;
            cdt_tri_t *nt = realloc(m->tris, (size_t)next_cap * sizeof(cdt_tri_t));
            if (nt) {
                m->tris = nt;
            }
            int32_t *nf = realloc(m->free_tris, (size_t)next_cap * sizeof(int32_t));
            if (nf) {
                m->free_tris = nf;
            }
            int32_t *ns = realloc(m->tri_visit_stamp, (size_t)next_cap * sizeof(int32_t));
            if (ns) {
                m->tri_visit_stamp = ns;
            }
            if (!nt || !nf || !ns) {
                return -1;
            }
            memset(m->tris + m->t_cap, 0, (size_t)(next_cap - m->t_cap) * sizeof(cdt_tri_t));
            memset(m->tri_visit_stamp + m->t_cap, 0,
                   (size_t)(next_cap - m->t_cap) * sizeof(int32_t));
            m->t_cap = next_cap;
        }
        tid = (int32_t)m->t_count++;
    }
    memset(&m->tris[tid], 0, sizeof(cdt_tri_t));
    m->tris[tid].adj[0] = -1;
    m->tris[tid].adj[1] = -1;
    m->tris[tid].adj[2] = -1;
    m->tris[tid].heap_idx = -1;
    m->tris[tid].flags = TRI_ALIVE;
    return tid;
}

static void cdt_heap_swap(cdt_mesh_t *m, uint32_t i, uint32_t j)
{
    int32_t ti = m->heap[i];
    int32_t tj = m->heap[j];
    m->heap[i] = tj;
    m->heap[j] = ti;
    m->tris[tj].heap_idx = (int32_t)i;
    m->tris[ti].heap_idx = (int32_t)j;
}

static void cdt_heap_sift_up(cdt_mesh_t *m, uint32_t idx)
{
    while (idx > 0u) {
        uint32_t parent = (idx - 1u) >> 1u;
        if (m->tris[m->heap[idx]].b2 > m->tris[m->heap[parent]].b2) {
            cdt_heap_swap(m, idx, parent);
            idx = parent;
        } else {
            break;
        }
    }
}

static void cdt_heap_sift_down(cdt_mesh_t *m, uint32_t idx)
{
    for (;;) {
        uint32_t left = 2u * idx + 1u;
        uint32_t right = left + 1u;
        uint32_t best = idx;
        if (left < m->heap_len && m->tris[m->heap[left]].b2 > m->tris[m->heap[best]].b2) {
            best = left;
        }
        if (right < m->heap_len && m->tris[m->heap[right]].b2 > m->tris[m->heap[best]].b2) {
            best = right;
        }
        if (best == idx) {
            break;
        }
        cdt_heap_swap(m, idx, best);
        idx = best;
    }
}

static void cdt_heap_remove(cdt_mesh_t *m, int32_t tid)
{
    int32_t hi = m->tris[tid].heap_idx;
    if (hi < 0) {
        return;
    }
    uint32_t idx = (uint32_t)hi;
    uint32_t last = --m->heap_len;
    m->tris[tid].heap_idx = -1;
    if (idx < last) {
        int32_t moved = m->heap[last];
        m->heap[idx] = moved;
        m->tris[moved].heap_idx = (int32_t)idx;
        cdt_heap_sift_up(m, idx);
        cdt_heap_sift_down(m, (uint32_t)m->tris[moved].heap_idx);
    }
}

static bool cdt_tri_is_refineable(const cdt_mesh_t *m, int32_t tid, double *out_b2)
{
    const cdt_tri_t *t = &m->tris[tid];
    if ((t->flags & (TRI_ALIVE | TRI_INTERIOR)) != (TRI_ALIVE | TRI_INTERIOR)) {
        return false;
    }
    if (m->verts[t->v[0]].is_super || m->verts[t->v[1]].is_super || m->verts[t->v[2]].is_super) {
        return false;
    }

    we_cdt_vec2_t p0 = cdt_vp(m, t->v[0]);
    we_cdt_vec2_t p1 = cdt_vp(m, t->v[1]);
    we_cdt_vec2_t p2 = cdt_vp(m, t->v[2]);

    double l2[3] = {
        dist2_pts(p0, p1), /* edge 0: v0->v1 */
        dist2_pts(p1, p2), /* edge 1: v1->v2 */
        dist2_pts(p2, p0), /* edge 2: v2->v0 */
    };
    double l_min2 = fmin(l2[0], fmin(l2[1], l2[2]));
    if (l_min2 <= m->min_edge_len2) {
        return false;
    }

    double area2 = (p1.x - p0.x) * (p2.y - p0.y) - (p1.y - p0.y) * (p2.x - p0.x);
    if (area2 <= 1e-18) {
        return false;
    }

    double b2 = (l2[0] * l2[1] * l2[2]) / (4.0 * area2 * area2 * l_min2);
    if (b2 <= m->b_max2) {
        return false;
    }

    /* Skip triangles locked between two constrained edges meeting at < theta_min */
    for (int e = 0; e < 3; e++) {
        int e_prev = (e + 2) % 3;
        if (((t->constrained >> e) & 1u) && ((t->constrained >> e_prev) & 1u)) {
            we_cdt_vec2_t apex = cdt_vp(m, t->v[e]);
            we_cdt_vec2_t a = cdt_vp(m, t->v[(e + 1) % 3]);
            we_cdt_vec2_t b = cdt_vp(m, t->v[e_prev]);
            double d1 = hypot(a.x - apex.x, a.y - apex.y);
            double d2 = hypot(b.x - apex.x, b.y - apex.y);
            if (d1 > 1e-12 && d2 > 1e-12) {
                double cos_a =
                    ((a.x - apex.x) * (b.x - apex.x) + (a.y - apex.y) * (b.y - apex.y)) / (d1 * d2);
                if (cos_a > m->cos_theta_min && fmax(d1, d2) <= 2.1 * fmin(d1, d2)) {
                    return false;
                }
            }
        }
    }

    *out_b2 = b2;
    return true;
}

static bool cdt_heap_update_tri(cdt_mesh_t *m, int32_t tid)
{
    double b2 = 0.0;
    bool bad = cdt_tri_is_refineable(m, tid, &b2);
    if (!bad) {
        cdt_heap_remove(m, tid);
        return true;
    }
    m->tris[tid].b2 = b2;
    if (m->tris[tid].heap_idx >= 0) {
        uint32_t idx = (uint32_t)m->tris[tid].heap_idx;
        cdt_heap_sift_up(m, idx);
        cdt_heap_sift_down(m, (uint32_t)m->tris[tid].heap_idx);
        return true;
    }
    if (m->heap_len == m->heap_cap) {
        if (m->heap_cap > UINT32_MAX / 2u) {
            return false;
        }
        uint32_t next_cap = m->heap_cap * 2u;
        int32_t *nh = realloc(m->heap, (size_t)next_cap * sizeof(int32_t));
        if (!nh) {
            return false;
        }
        m->heap = nh;
        m->heap_cap = next_cap;
    }
    uint32_t idx = m->heap_len++;
    m->heap[idx] = tid;
    m->tris[tid].heap_idx = (int32_t)idx;
    cdt_heap_sift_up(m, idx);
    return true;
}

static int cdt_find_neighbor_edge(const cdt_mesh_t *m, int32_t tn, int32_t target_tri)
{
    if (tn < 0) {
        return -1;
    }
    for (int e = 0; e < 3; e++) {
        if (m->tris[tn].adj[e] == target_tri) {
            return e;
        }
    }
    return -1;
}

static void cdt_replace_neighbor(cdt_mesh_t *m, int32_t tn, int32_t old_t, int32_t new_t)
{
    if (tn < 0) {
        return;
    }
    for (int e = 0; e < 3; e++) {
        if (m->tris[tn].adj[e] == old_t) {
            m->tris[tn].adj[e] = new_t;
            return;
        }
    }
}

static bool cdt_push_flip(cdt_mesh_t *m, uint32_t *sp, int32_t tri, int32_t edge)
{
    if (*sp == m->flip_cap) {
        if (m->flip_cap > UINT32_MAX / 2u) {
            return false;
        }
        uint32_t next_cap = m->flip_cap * 2u;
        cdt_edge_ref_t *ns = realloc(m->flip_stack, (size_t)next_cap * sizeof(cdt_edge_ref_t));
        if (!ns) {
            return false;
        }
        m->flip_stack = ns;
        m->flip_cap = next_cap;
    }
    m->flip_stack[(*sp)++] = (cdt_edge_ref_t){tri, edge};
    return true;
}

static bool cdt_flip_edge(cdt_mesh_t *m, int32_t t, int e, bool track_heap)
{
    int32_t tn = m->tris[t].adj[e];
    int en = cdt_find_neighbor_edge(m, tn, t);

    int32_t a = m->tris[t].v[e];
    int32_t b = m->tris[t].v[(e + 1) % 3];
    int32_t c = m->tris[t].v[(e + 2) % 3];
    int32_t d = m->tris[tn].v[(en + 2) % 3];

    int32_t adj_bc = m->tris[t].adj[(e + 1) % 3];
    uint8_t con_bc = (uint8_t)((m->tris[t].constrained >> ((e + 1) % 3)) & 1u);
    int32_t adj_ca = m->tris[t].adj[(e + 2) % 3];
    uint8_t con_ca = (uint8_t)((m->tris[t].constrained >> ((e + 2) % 3)) & 1u);

    int32_t adj_ad = m->tris[tn].adj[(en + 1) % 3];
    uint8_t con_ad = (uint8_t)((m->tris[tn].constrained >> ((en + 1) % 3)) & 1u);
    int32_t adj_db = m->tris[tn].adj[(en + 2) % 3];
    uint8_t con_db = (uint8_t)((m->tris[tn].constrained >> ((en + 2) % 3)) & 1u);

    uint8_t flags_t = m->tris[t].flags;
    uint8_t flags_tn = m->tris[tn].flags;

    m->tris[t].v[0] = c;
    m->tris[t].v[1] = d;
    m->tris[t].v[2] = b;
    m->tris[t].adj[0] = tn;
    m->tris[t].adj[1] = adj_db;
    m->tris[t].adj[2] = adj_bc;
    m->tris[t].constrained = (uint8_t)((con_db << 1u) | (con_bc << 2u));
    m->tris[t].flags = flags_t;

    m->tris[tn].v[0] = d;
    m->tris[tn].v[1] = c;
    m->tris[tn].v[2] = a;
    m->tris[tn].adj[0] = t;
    m->tris[tn].adj[1] = adj_ca;
    m->tris[tn].adj[2] = adj_ad;
    m->tris[tn].constrained = (uint8_t)((con_ca << 1u) | (con_ad << 2u));
    m->tris[tn].flags = flags_tn;

    cdt_replace_neighbor(m, adj_db, tn, t);
    cdt_replace_neighbor(m, adj_ca, t, tn);

    m->verts[a].any_tri = tn;
    m->verts[b].any_tri = t;
    m->verts[c].any_tri = t;
    m->verts[d].any_tri = tn;

    if (track_heap) {
        if (!cdt_heap_update_tri(m, t) || !cdt_heap_update_tri(m, tn)) {
            return false;
        }
    }
    return true;
}

static bool cdt_legalize_from_stack(cdt_mesh_t *m, uint32_t sp, bool track_heap)
{
    uint32_t max_flips = (m->t_count + 64u) * 16u;
    uint32_t flips = 0u;
    while (sp > 0u) {
        cdt_edge_ref_t ref = m->flip_stack[--sp];
        int32_t t = ref.tri;
        int e = ref.edge;
        if (!(m->tris[t].flags & TRI_ALIVE)) {
            continue;
        }
        if ((m->tris[t].constrained >> e) & 1u) {
            continue;
        }
        int32_t tn = m->tris[t].adj[e];
        if (tn < 0 || !(m->tris[tn].flags & TRI_ALIVE)) {
            continue;
        }
        int en = cdt_find_neighbor_edge(m, tn, t);
        if (en < 0) {
            continue;
        }

        we_cdt_vec2_t pa = cdt_vp(m, m->tris[t].v[e]);
        we_cdt_vec2_t pb = cdt_vp(m, m->tris[t].v[(e + 1) % 3]);
        we_cdt_vec2_t pc = cdt_vp(m, m->tris[t].v[(e + 2) % 3]);
        we_cdt_vec2_t pd = cdt_vp(m, m->tris[tn].v[(en + 2) % 3]);

        if (incircle(pa, pb, pc, pd) > 0.0) {
            if (flips++ > max_flips) {
                return false;
            }
            if (!cdt_flip_edge(m, t, e, track_heap)) {
                return false;
            }
            /* cdt_flip_edge places c (the inserted vertex opposite e) at t.v[0] and tn.v[1],
               so the outer edges opposite c after the flip are edge 1 of t (d->b) and edge 2 of tn
               (a->d). */
            if (!cdt_push_flip(m, &sp, t, 1) || !cdt_push_flip(m, &sp, tn, 2)) {
                return false;
            }
        }
    }
    return true;
}

static int32_t cdt_locate_point(cdt_mesh_t *m, int32_t hint_tri, we_cdt_vec2_t p, int *out_on_edge,
                                int32_t *out_dup_vert)
{
    *out_on_edge = -1;
    *out_dup_vert = -1;

    int32_t t = hint_tri;
    if (t < 0 || (uint32_t)t >= m->t_count || !(m->tris[t].flags & TRI_ALIVE)) {
        t = 0;
        while ((uint32_t)t < m->t_count && !(m->tris[t].flags & TRI_ALIVE)) {
            t++;
        }
    }

    uint32_t max_steps = m->t_count + 16u;
    for (uint32_t step = 0u; step < max_steps; step++) {
        const cdt_tri_t *tr = &m->tris[t];
        int32_t next_t = -1;
        bool outside_hull = false;
        for (int e = 0; e < 3; e++) {
            we_cdt_vec2_t a = cdt_vp(m, tr->v[e]);
            we_cdt_vec2_t b = cdt_vp(m, tr->v[(e + 1) % 3]);
            if (orient2d(a, b, p) < 0.0) {
                if (tr->adj[e] >= 0) {
                    next_t = tr->adj[e];
                    break;
                }
                outside_hull = true;
            }
        }
        if (next_t < 0) {
            if (outside_hull) {
                return -1;
            }
            goto found;
        }
        t = next_t;
    }

    /* Fallback linear scan if walk hit a non-Delaunay loop during recovery */
    for (uint32_t idx = 0u; idx < m->t_count; idx++) {
        const cdt_tri_t *tr = &m->tris[idx];
        if (!(tr->flags & TRI_ALIVE)) {
            continue;
        }
        we_cdt_vec2_t p0 = cdt_vp(m, tr->v[0]);
        we_cdt_vec2_t p1 = cdt_vp(m, tr->v[1]);
        we_cdt_vec2_t p2 = cdt_vp(m, tr->v[2]);
        if (orient2d(p0, p1, p) >= 0.0 && orient2d(p1, p2, p) >= 0.0 &&
            orient2d(p2, p0, p) >= 0.0) {
            t = (int32_t)idx;
            goto found;
        }
    }
    return -1;

found:
    for (int k = 0; k < 3; k++) {
        int32_t vid = m->tris[t].v[k];
        if (dist2_pts(cdt_vp(m, vid), p) <= m->dup_dist2) {
            *out_dup_vert = vid;
            return t;
        }
    }
    for (int e = 0; e < 3; e++) {
        we_cdt_vec2_t a = cdt_vp(m, m->tris[t].v[e]);
        we_cdt_vec2_t b = cdt_vp(m, m->tris[t].v[(e + 1) % 3]);
        if (orient2d(a, b, p) == 0.0) {
            *out_on_edge = e;
            return t;
        }
    }
    return t;
}

static bool cdt_split_tri_1to3(cdt_mesh_t *m, int32_t t, int32_t vid, bool track_heap)
{
    int32_t t1 = cdt_alloc_tri(m);
    int32_t t2 = cdt_alloc_tri(m);
    if (t1 < 0 || t2 < 0) {
        return false;
    }

    int32_t v0 = m->tris[t].v[0];
    int32_t v1 = m->tris[t].v[1];
    int32_t v2 = m->tris[t].v[2];
    int32_t a0 = m->tris[t].adj[0];
    int32_t a1 = m->tris[t].adj[1];
    int32_t a2 = m->tris[t].adj[2];
    uint8_t c0 = (uint8_t)(m->tris[t].constrained & 1u);
    uint8_t c1 = (uint8_t)((m->tris[t].constrained >> 1u) & 1u);
    uint8_t c2 = (uint8_t)((m->tris[t].constrained >> 2u) & 1u);
    uint8_t flags = m->tris[t].flags;

    m->tris[t].v[0] = v0;
    m->tris[t].v[1] = v1;
    m->tris[t].v[2] = vid;
    m->tris[t].adj[0] = a0;
    m->tris[t].adj[1] = t1;
    m->tris[t].adj[2] = t2;
    m->tris[t].constrained = c0;
    m->tris[t].flags = flags;

    m->tris[t1].v[0] = v1;
    m->tris[t1].v[1] = v2;
    m->tris[t1].v[2] = vid;
    m->tris[t1].adj[0] = a1;
    m->tris[t1].adj[1] = t2;
    m->tris[t1].adj[2] = t;
    m->tris[t1].constrained = c1;
    m->tris[t1].flags = flags;

    m->tris[t2].v[0] = v2;
    m->tris[t2].v[1] = v0;
    m->tris[t2].v[2] = vid;
    m->tris[t2].adj[0] = a2;
    m->tris[t2].adj[1] = t;
    m->tris[t2].adj[2] = t1;
    m->tris[t2].constrained = c2;
    m->tris[t2].flags = flags;

    cdt_replace_neighbor(m, a1, t, t1);
    cdt_replace_neighbor(m, a2, t, t2);

    m->verts[v0].any_tri = t;
    m->verts[v1].any_tri = t1;
    m->verts[v2].any_tri = t2;
    m->verts[vid].any_tri = t;

    if (track_heap) {
        if (!cdt_heap_update_tri(m, t) || !cdt_heap_update_tri(m, t1) ||
            !cdt_heap_update_tri(m, t2)) {
            return false;
        }
    }

    uint32_t sp = 0u;
    if (!cdt_push_flip(m, &sp, t, 0) || !cdt_push_flip(m, &sp, t1, 0) ||
        !cdt_push_flip(m, &sp, t2, 0)) {
        return false;
    }
    return cdt_legalize_from_stack(m, sp, track_heap);
}

static bool cdt_split_edge_2to4(cdt_mesh_t *m, int32_t t, int e, int32_t vid, bool track_heap,
                                int32_t out_new_tris[4])
{
    int32_t a = m->tris[t].v[e];
    int32_t b = m->tris[t].v[(e + 1) % 3];
    int32_t c = m->tris[t].v[(e + 2) % 3];
    int32_t a1 = m->tris[t].adj[(e + 1) % 3];
    int32_t a2 = m->tris[t].adj[(e + 2) % 3];
    uint8_t e_con = (uint8_t)((m->tris[t].constrained >> e) & 1u);
    uint8_t c1 = (uint8_t)((m->tris[t].constrained >> ((e + 1) % 3)) & 1u);
    uint8_t c2 = (uint8_t)((m->tris[t].constrained >> ((e + 2) % 3)) & 1u);
    uint8_t flags_t = m->tris[t].flags;

    int32_t tn = m->tris[t].adj[e];
    if (tn < 0) {
        int32_t t1 = cdt_alloc_tri(m);
        if (t1 < 0) {
            return false;
        }
        m->tris[t].v[0] = a;
        m->tris[t].v[1] = vid;
        m->tris[t].v[2] = c;
        m->tris[t].adj[0] = -1;
        m->tris[t].adj[1] = t1;
        m->tris[t].adj[2] = a2;
        m->tris[t].constrained = (uint8_t)(e_con | (c2 << 2u));
        m->tris[t].flags = flags_t;

        m->tris[t1].v[0] = vid;
        m->tris[t1].v[1] = b;
        m->tris[t1].v[2] = c;
        m->tris[t1].adj[0] = -1;
        m->tris[t1].adj[1] = a1;
        m->tris[t1].adj[2] = t;
        m->tris[t1].constrained = (uint8_t)(e_con | (c1 << 1u));
        m->tris[t1].flags = flags_t;

        cdt_replace_neighbor(m, a1, t, t1);
        m->verts[a].any_tri = t;
        m->verts[b].any_tri = t1;
        m->verts[c].any_tri = t;
        m->verts[vid].any_tri = t;

        if (out_new_tris) {
            out_new_tris[0] = t;
            out_new_tris[1] = t1;
            out_new_tris[2] = -1;
            out_new_tris[3] = -1;
        }
        if (track_heap) {
            if (!cdt_heap_update_tri(m, t) || !cdt_heap_update_tri(m, t1)) {
                return false;
            }
        }
        uint32_t sp = 0u;
        if (!cdt_push_flip(m, &sp, t, 2) || !cdt_push_flip(m, &sp, t1, 1)) {
            return false;
        }
        return cdt_legalize_from_stack(m, sp, track_heap);
    }

    int en = cdt_find_neighbor_edge(m, tn, t);
    int32_t d = m->tris[tn].v[(en + 2) % 3];
    int32_t an1 = m->tris[tn].adj[(en + 1) % 3];
    int32_t an2 = m->tris[tn].adj[(en + 2) % 3];
    uint8_t cn1 = (uint8_t)((m->tris[tn].constrained >> ((en + 1) % 3)) & 1u);
    uint8_t cn2 = (uint8_t)((m->tris[tn].constrained >> ((en + 2) % 3)) & 1u);
    uint8_t flags_tn = m->tris[tn].flags;

    int32_t t1 = cdt_alloc_tri(m);
    int32_t t3 = cdt_alloc_tri(m);
    if (t1 < 0 || t3 < 0) {
        return false;
    }

    /* t0 = t: (a, vid, c) with c at v[2] (opp edge 2 is c->a) */
    m->tris[t].v[0] = a;
    m->tris[t].v[1] = vid;
    m->tris[t].v[2] = c;
    m->tris[t].adj[0] = tn;
    m->tris[t].adj[1] = t1;
    m->tris[t].adj[2] = a2;
    m->tris[t].constrained = (uint8_t)(e_con | (c2 << 2u));
    m->tris[t].flags = flags_t;

    /* t1: (vid, b, c) with c at v[2] (opp edge 1 is b->c) */
    m->tris[t1].v[0] = vid;
    m->tris[t1].v[1] = b;
    m->tris[t1].v[2] = c;
    m->tris[t1].adj[0] = t3;
    m->tris[t1].adj[1] = a1;
    m->tris[t1].adj[2] = t;
    m->tris[t1].constrained = (uint8_t)(e_con | (c1 << 1u));
    m->tris[t1].flags = flags_t;

    /* t2 = tn: (vid, a, d) with d at v[2] (opp edge 1 is a->d) */
    m->tris[tn].v[0] = vid;
    m->tris[tn].v[1] = a;
    m->tris[tn].v[2] = d;
    m->tris[tn].adj[0] = t;
    m->tris[tn].adj[1] = an1;
    m->tris[tn].adj[2] = t3;
    m->tris[tn].constrained = (uint8_t)(e_con | (cn1 << 1u));
    m->tris[tn].flags = flags_tn;

    /* t3: (b, vid, d) with d at v[2] (opp edge 2 is d->b) */
    m->tris[t3].v[0] = b;
    m->tris[t3].v[1] = vid;
    m->tris[t3].v[2] = d;
    m->tris[t3].adj[0] = t1;
    m->tris[t3].adj[1] = tn;
    m->tris[t3].adj[2] = an2;
    m->tris[t3].constrained = (uint8_t)(e_con | (cn2 << 2u));
    m->tris[t3].flags = flags_tn;

    cdt_replace_neighbor(m, a1, t, t1);
    cdt_replace_neighbor(m, an2, tn, t3);

    m->verts[a].any_tri = t;
    m->verts[b].any_tri = t1;
    m->verts[c].any_tri = t;
    m->verts[d].any_tri = tn;
    m->verts[vid].any_tri = t;

    if (out_new_tris) {
        out_new_tris[0] = t;
        out_new_tris[1] = t1;
        out_new_tris[2] = tn;
        out_new_tris[3] = t3;
    }

    if (track_heap) {
        if (!cdt_heap_update_tri(m, t) || !cdt_heap_update_tri(m, t1) ||
            !cdt_heap_update_tri(m, tn) || !cdt_heap_update_tri(m, t3)) {
            return false;
        }
    }

    /* Each pushed edge must have vid at v[(e+2)%3] so cdt_legalize_from_stack tracks the
       two outer edges opposite vid after cdt_flip_edge. */
    uint32_t sp = 0u;
    if (!cdt_push_flip(m, &sp, t, 2) || !cdt_push_flip(m, &sp, t1, 1) ||
        !cdt_push_flip(m, &sp, tn, 1) || !cdt_push_flip(m, &sp, t3, 2)) {
        return false;
    }
    return cdt_legalize_from_stack(m, sp, track_heap);
}

static int32_t cdt_insert_point(cdt_mesh_t *m, we_cdt_vec2_t p, int32_t hint_tri, bool is_boundary,
                                bool is_pin, bool track_heap, bool *out_inserted_new)
{
    if (out_inserted_new) {
        *out_inserted_new = false;
    }
    int on_edge = -1;
    int32_t dup_vert = -1;
    int32_t t = cdt_locate_point(m, hint_tri, p, &on_edge, &dup_vert);
    if (t < 0) {
        return -1;
    }
    if (dup_vert >= 0) {
        if (is_boundary) {
            m->verts[dup_vert].is_boundary = true;
        }
        if (is_pin) {
            m->verts[dup_vert].is_pin = true;
        }
        return dup_vert;
    }
    if (m->v_count - 3u >= m->max_non_super_verts) {
        m->hit_vertex_budget = true;
        return -1;
    }

    int32_t vid = (int32_t)m->v_count++;
    m->verts[vid].x = p.x;
    m->verts[vid].y = p.y;
    m->verts[vid].is_super = false;
    m->verts[vid].is_boundary = is_boundary;
    m->verts[vid].is_pin = is_pin;
    m->verts[vid].has_acute_corner = false;
    m->verts[vid].any_tri = t;

    bool ok;
    if (on_edge >= 0) {
        ok = cdt_split_edge_2to4(m, t, on_edge, vid, track_heap, NULL);
    } else {
        ok = cdt_split_tri_1to3(m, t, vid, track_heap);
    }
    if (!ok) {
        return -1;
    }
    if (out_inserted_new) {
        *out_inserted_new = true;
    }
    return vid;
}

static bool cdt_find_edge_between(const cdt_mesh_t *m, int32_t va, int32_t vb, int32_t *out_tri,
                                  int *out_edge)
{
    int32_t start_t = m->verts[va].any_tri;
    if (start_t >= 0 && (m->tris[start_t].flags & TRI_ALIVE)) {
        /* Fan walk around va in both directions */
        for (int dir = 0; dir < 2; dir++) {
            int32_t t = start_t;
            uint32_t guard = 0u;
            while (t >= 0 && guard++ < m->t_count + 4u) {
                const cdt_tri_t *tr = &m->tris[t];
                int idx = -1;
                for (int k = 0; k < 3; k++) {
                    if (tr->v[k] == va) {
                        idx = k;
                        break;
                    }
                }
                if (idx < 0) {
                    break;
                }
                int e_out = idx;
                int e_in = (idx + 2) % 3;
                if (tr->v[(idx + 1) % 3] == vb) {
                    *out_tri = t;
                    *out_edge = e_out;
                    return true;
                }
                if (tr->v[(idx + 2) % 3] == vb) {
                    *out_tri = t;
                    *out_edge = e_in;
                    return true;
                }
                int32_t next_t = (dir == 0) ? tr->adj[e_out] : tr->adj[e_in];
                if (next_t == start_t) {
                    break;
                }
                t = next_t;
            }
        }
    }

    for (uint32_t tid = 0u; tid < m->t_count; tid++) {
        const cdt_tri_t *tr = &m->tris[tid];
        if (!(tr->flags & TRI_ALIVE)) {
            continue;
        }
        for (int e = 0; e < 3; e++) {
            int32_t u = tr->v[e];
            int32_t w = tr->v[(e + 1) % 3];
            if ((u == va && w == vb) || (u == vb && w == va)) {
                *out_tri = (int32_t)tid;
                *out_edge = e;
                return true;
            }
        }
    }
    return false;
}

static void cdt_mark_edge_constrained(cdt_mesh_t *m, int32_t t, int e)
{
    m->tris[t].constrained |= (uint8_t)(1u << e);
    int32_t tn = m->tris[t].adj[e];
    if (tn >= 0) {
        int en = cdt_find_neighbor_edge(m, tn, t);
        if (en >= 0) {
            m->tris[tn].constrained |= (uint8_t)(1u << en);
        }
    }
}

static bool cdt_recover_segment_sloan(cdt_mesh_t *m, int32_t va, int32_t vb)
{
    typedef struct {
        int32_t a;
        int32_t b;
    } seg_task_t;

    uint32_t task_cap = 32u;
    uint32_t task_sp = 0u;
    seg_task_t *tasks = malloc(task_cap * sizeof(seg_task_t));
    uint32_t q_cap = 128u;
    cdt_seg_pair_t *q_cross = malloc(q_cap * sizeof(cdt_seg_pair_t));
    cdt_seg_pair_t *new_edges = malloc(q_cap * sizeof(cdt_seg_pair_t));
    if (!tasks || !q_cross || !new_edges) {
        free(new_edges);
        free(q_cross);
        free(tasks);
        return false;
    }

    tasks[task_sp++] = (seg_task_t){va, vb};
    bool ok = true;
    uint32_t new_cap = q_cap;

    while (task_sp > 0u && ok) {
        seg_task_t cur = tasks[--task_sp];
        int32_t a = cur.a;
        int32_t b = cur.b;
        if (a == b) {
            continue;
        }

        int32_t et = -1;
        int ee = -1;
        if (cdt_find_edge_between(m, a, b, &et, &ee)) {
            cdt_mark_edge_constrained(m, et, ee);
            continue;
        }

        we_cdt_vec2_t pa = cdt_vp(m, a);
        we_cdt_vec2_t pb = cdt_vp(m, b);

        /* Find first triangle around vertex a whose opposite edge crosses segment a->b */
        int32_t walk_t = -1;
        int32_t opp_u = -1;
        int32_t opp_w = -1;
        int32_t col_v = -1;

        for (uint32_t tid = 0u; tid < m->t_count; tid++) {
            const cdt_tri_t *tr = &m->tris[tid];
            if (!(tr->flags & TRI_ALIVE)) {
                continue;
            }
            int k = -1;
            for (int j = 0; j < 3; j++) {
                if (tr->v[j] == a) {
                    k = j;
                    break;
                }
            }
            if (k < 0) {
                continue;
            }
            int32_t vp = tr->v[(k + 1) % 3];
            int32_t vq = tr->v[(k + 2) % 3];
            double o_apb = orient2d(pa, cdt_vp(m, vp), pb);
            double o_abq = orient2d(pa, pb, cdt_vp(m, vq));
            if (o_apb >= 0.0 && o_abq >= 0.0) {
                if (o_apb == 0.0) {
                    we_cdt_vec2_t pvp = cdt_vp(m, vp);
                    if ((pvp.x - pa.x) * (pb.x - pa.x) + (pvp.y - pa.y) * (pb.y - pa.y) > 0.0) {
                        col_v = vp;
                        break;
                    }
                } else if (o_abq == 0.0) {
                    we_cdt_vec2_t pvq = cdt_vp(m, vq);
                    if ((pvq.x - pa.x) * (pb.x - pa.x) + (pvq.y - pa.y) * (pb.y - pa.y) > 0.0) {
                        col_v = vq;
                        break;
                    }
                } else {
                    walk_t = (int32_t)tid;
                    opp_u = vp;
                    opp_w = vq;
                    break;
                }
            }
        }

        if (col_v >= 0) {
            if (task_sp + 2u > task_cap) {
                task_cap *= 2u;
                seg_task_t *nt = realloc(tasks, task_cap * sizeof(seg_task_t));
                if (!nt) {
                    ok = false;
                    break;
                }
                tasks = nt;
            }
            tasks[task_sp++] = (seg_task_t){a, col_v};
            tasks[task_sp++] = (seg_task_t){col_v, b};
            continue;
        }

        if (walk_t < 0) {
            ok = false;
            break;
        }

        uint32_t q_len = 0u;
        uint32_t walk_guard = 0u;
        int32_t prev_u = opp_u;
        int32_t prev_w = opp_w;
        int32_t cur_t = walk_t;

        for (;;) {
            if (walk_guard++ > m->t_count + 8u) {
                ok = false;
                break;
            }
            /* Find edge (prev_u, prev_w) in cur_t */
            int edge_idx = -1;
            for (int e = 0; e < 3; e++) {
                if (m->tris[cur_t].v[e] == prev_u && m->tris[cur_t].v[(e + 1) % 3] == prev_w) {
                    edge_idx = e;
                    break;
                }
            }
            if (edge_idx < 0) {
                ok = false;
                break;
            }
            if ((m->tris[cur_t].constrained >> edge_idx) & 1u) {
                /* Crossing an existing constrained edge means invalid intersecting loops */
                ok = false;
                break;
            }
            if (q_len == q_cap) {
                q_cap *= 2u;
                cdt_seg_pair_t *nq = realloc(q_cross, q_cap * sizeof(cdt_seg_pair_t));
                if (!nq) {
                    ok = false;
                    break;
                }
                q_cross = nq;
            }
            q_cross[q_len++] = (cdt_seg_pair_t){prev_u, prev_w};

            int32_t tn = m->tris[cur_t].adj[edge_idx];
            if (tn < 0) {
                ok = false;
                break;
            }
            int en = cdt_find_neighbor_edge(m, tn, cur_t);
            int32_t r = m->tris[tn].v[(en + 2) % 3];
            if (r == b) {
                break;
            }
            double s = orient2d(pa, pb, cdt_vp(m, r));
            if (s == 0.0) {
                col_v = r;
                break;
            }
            cur_t = tn;
            if (s > 0.0) {
                /* r is left of a->b (same side as prev_w); crossed edge is (prev_u, r) */
                prev_w = r;
            } else {
                /* r is right of a->b (same side as prev_u); crossed edge is (r, prev_w) */
                prev_u = r;
            }
        }

        if (!ok) {
            break;
        }
        if (col_v >= 0) {
            if (task_sp + 2u > task_cap) {
                task_cap *= 2u;
                seg_task_t *nt = realloc(tasks, task_cap * sizeof(seg_task_t));
                if (!nt) {
                    ok = false;
                    break;
                }
                tasks = nt;
            }
            tasks[task_sp++] = (seg_task_t){a, col_v};
            tasks[task_sp++] = (seg_task_t){col_v, b};
            continue;
        }

        uint32_t new_len = 0u;
        uint64_t flip_budget = (uint64_t)q_len * (uint64_t)q_len * 2u;
        uint64_t min_budget = 16ULL * (uint64_t)(m->v_count > 64u ? m->v_count : 64u);
        if (flip_budget < min_budget) {
            flip_budget = min_budget;
        }
        uint32_t no_flip_streak = 0u;
        uint32_t q_head = 0u;

        while (q_head < q_len) {
            uint32_t remaining = q_len - q_head;
            if (no_flip_streak > remaining || flip_budget == 0u) {
                ok = false;
                break;
            }
            if (q_head > 4096u && q_head * 2u >= q_len) {
                memmove(q_cross, q_cross + q_head, (size_t)remaining * sizeof(cdt_seg_pair_t));
                q_head = 0u;
                q_len = remaining;
            }
            cdt_seg_pair_t seg = q_cross[q_head++];
            int32_t tf = -1;
            int ef = -1;
            if (!cdt_find_edge_between(m, seg.va, seg.vb, &tf, &ef)) {
                continue;
            }
            if ((m->tris[tf].constrained >> ef) & 1u) {
                ok = false;
                break;
            }
            int32_t tn = m->tris[tf].adj[ef];
            if (tn < 0) {
                continue;
            }
            int en = cdt_find_neighbor_edge(m, tn, tf);
            int32_t u = m->tris[tf].v[ef];
            int32_t w = m->tris[tf].v[(ef + 1) % 3];
            int32_t c = m->tris[tf].v[(ef + 2) % 3];
            int32_t d = m->tris[tn].v[(en + 2) % 3];

            we_cdt_vec2_t pu = cdt_vp(m, u);
            we_cdt_vec2_t pw = cdt_vp(m, w);
            we_cdt_vec2_t pc = cdt_vp(m, c);
            we_cdt_vec2_t pd = cdt_vp(m, d);

            /* Strictly convex quad (u, d, w, c) iff u is right of c->d and w is left of c->d */
            if (!(orient2d(pc, pd, pu) < 0.0 && orient2d(pc, pd, pw) > 0.0)) {
                no_flip_streak++;
                if (q_len == q_cap) {
                    if (q_cap > UINT32_MAX / 2u) {
                        ok = false;
                        break;
                    }
                    q_cap *= 2u;
                    cdt_seg_pair_t *nq = realloc(q_cross, (size_t)q_cap * sizeof(cdt_seg_pair_t));
                    if (!nq) {
                        ok = false;
                        break;
                    }
                    q_cross = nq;
                }
                q_cross[q_len++] = seg;
                continue;
            }

            no_flip_streak = 0u;
            flip_budget--;
            if (!cdt_flip_edge(m, tf, ef, false)) {
                ok = false;
                break;
            }

            bool still_crosses = false;
            if (c != a && c != b && d != a && d != b) {
                double oc = orient2d(pa, pb, pc);
                double od = orient2d(pa, pb, pd);
                if ((oc > 0.0 && od < 0.0) || (oc < 0.0 && od > 0.0)) {
                    still_crosses = true;
                }
            }

            if (still_crosses) {
                if (q_len == q_cap) {
                    q_cap *= 2u;
                    cdt_seg_pair_t *nq = realloc(q_cross, q_cap * sizeof(cdt_seg_pair_t));
                    if (!nq) {
                        ok = false;
                        break;
                    }
                    q_cross = nq;
                }
                q_cross[q_len++] = (cdt_seg_pair_t){c, d};
            } else {
                if ((c == a && d == b) || (c == b && d == a)) {
                    int32_t mf = -1;
                    int me = -1;
                    if (cdt_find_edge_between(m, a, b, &mf, &me)) {
                        cdt_mark_edge_constrained(m, mf, me);
                    }
                } else {
                    if (new_len == new_cap) {
                        new_cap *= 2u;
                        cdt_seg_pair_t *nne = realloc(new_edges, new_cap * sizeof(cdt_seg_pair_t));
                        if (!nne) {
                            ok = false;
                            break;
                        }
                        new_edges = nne;
                    }
                    new_edges[new_len++] = (cdt_seg_pair_t){c, d};
                }
            }
        }

        if (!ok) {
            break;
        }

        /* Ensure (a, b) is marked constrained */
        if (cdt_find_edge_between(m, a, b, &et, &ee)) {
            cdt_mark_edge_constrained(m, et, ee);
        } else {
            ok = false;
            break;
        }

        /* Sloan local Delaunay restoration on newly flipped unconstrained edges */
        bool swapped = true;
        uint32_t restore_passes = 0u;
        while (swapped && ok) {
            if (restore_passes++ > 16u * (new_len + 16u)) {
                break;
            }
            swapped = false;
            for (uint32_t idx = 0u; idx < new_len; idx++) {
                int32_t tf = -1;
                int ef = -1;
                if (!cdt_find_edge_between(m, new_edges[idx].va, new_edges[idx].vb, &tf, &ef)) {
                    continue;
                }
                if ((m->tris[tf].constrained >> ef) & 1u) {
                    continue;
                }
                int32_t tn = m->tris[tf].adj[ef];
                if (tn < 0) {
                    continue;
                }
                int en = cdt_find_neighbor_edge(m, tn, tf);
                int32_t u = m->tris[tf].v[ef];
                int32_t w = m->tris[tf].v[(ef + 1) % 3];
                int32_t c = m->tris[tf].v[(ef + 2) % 3];
                int32_t d = m->tris[tn].v[(en + 2) % 3];
                if (incircle(cdt_vp(m, u), cdt_vp(m, w), cdt_vp(m, c), cdt_vp(m, d)) > 0.0) {
                    if (!cdt_flip_edge(m, tf, ef, false)) {
                        ok = false;
                        break;
                    }
                    new_edges[idx] = (cdt_seg_pair_t){c, d};
                    swapped = true;
                }
            }
        }
    }

    free(new_edges);
    free(q_cross);
    free(tasks);
    return ok;
}

static bool cdt_flood_fill_interior(cdt_mesh_t *m)
{
    int32_t *queue = malloc(m->t_count * sizeof(int32_t));
    if (!queue) {
        return false;
    }

    int32_t stamp = ++m->cur_stamp;
    int32_t root = m->verts[0].any_tri;
    if (root < 0 || !(m->tris[root].flags & TRI_ALIVE)) {
        free(queue);
        return false;
    }

    uint32_t head = 0u;
    uint32_t tail = 0u;
    m->tris[root].flags &= (uint8_t)(~TRI_INTERIOR);
    m->tri_visit_stamp[root] = stamp;
    queue[tail++] = root;

    while (head < tail) {
        int32_t t = queue[head++];
        bool cur_int = (m->tris[t].flags & TRI_INTERIOR) != 0u;
        for (int e = 0; e < 3; e++) {
            int32_t tn = m->tris[t].adj[e];
            if (tn < 0 || !(m->tris[tn].flags & TRI_ALIVE) || m->tri_visit_stamp[tn] == stamp) {
                continue;
            }
            bool cross_con = ((m->tris[t].constrained >> e) & 1u) != 0u;
            bool next_int = cross_con ? !cur_int : cur_int;
            if (next_int) {
                m->tris[tn].flags |= TRI_INTERIOR;
            } else {
                m->tris[tn].flags &= (uint8_t)(~TRI_INTERIOR);
            }
            m->tri_visit_stamp[tn] = stamp;
            queue[tail++] = tn;
        }
    }
    free(queue);

    /* Ensure no interior triangle touches a super-triangle vertex */
    uint32_t int_count = 0u;
    for (uint32_t tid = 0u; tid < m->t_count; tid++) {
        if ((m->tris[tid].flags & (TRI_ALIVE | TRI_INTERIOR)) == (TRI_ALIVE | TRI_INTERIOR)) {
            if (m->verts[m->tris[tid].v[0]].is_super || m->verts[m->tris[tid].v[1]].is_super ||
                m->verts[m->tris[tid].v[2]].is_super) {
                return false;
            }
            int_count++;
        }
    }
    return int_count > 0u;
}

/* =========================================================================
 * Bridson (2007) Poisson-Disk Interior Point Seeding
 * ========================================================================= */

typedef struct {
    int64_t gx;
    int64_t gy;
    int32_t vid;
    bool occupied;
} bridson_slot_t;

typedef struct {
    bridson_slot_t *slots;
    uint32_t mask;
    double inv_cell;
} bridson_hash_t;

static uint64_t hash_grid_coord(int64_t gx, int64_t gy)
{
    uint64_t h = (uint64_t)gx * 0x9E3779B97F4A7C15ULL ^ ((uint64_t)gy * 0xBF58476D1CE4E5B9ULL);
    h ^= h >> 30u;
    h *= 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 27u;
    return h;
}

static bool bridson_hash_insert(bridson_hash_t *h, double x, double y, int32_t vid)
{
    int64_t gx = (int64_t)floor(x * h->inv_cell);
    int64_t gy = (int64_t)floor(y * h->inv_cell);
    uint32_t idx = (uint32_t)hash_grid_coord(gx, gy) & h->mask;
    for (;;) {
        if (!h->slots[idx].occupied) {
            h->slots[idx].gx = gx;
            h->slots[idx].gy = gy;
            h->slots[idx].vid = vid;
            h->slots[idx].occupied = true;
            return true;
        }
        idx = (idx + 1u) & h->mask;
    }
}

static bool bridson_hash_far_enough(const bridson_hash_t *h, const cdt_mesh_t *m, double x,
                                    double y, double r2)
{
    int64_t gx = (int64_t)floor(x * h->inv_cell);
    int64_t gy = (int64_t)floor(y * h->inv_cell);
    we_cdt_vec2_t p = {x, y};
    for (int64_t dy = -2; dy <= 2; dy++) {
        for (int64_t dx = -2; dx <= 2; dx++) {
            int64_t cx = gx + dx;
            int64_t cy = gy + dy;
            uint32_t idx = (uint32_t)hash_grid_coord(cx, cy) & h->mask;
            while (h->slots[idx].occupied) {
                if (h->slots[idx].gx == cx && h->slots[idx].gy == cy) {
                    if (dist2_pts(p, cdt_vp(m, h->slots[idx].vid)) < r2) {
                        return false;
                    }
                }
                idx = (idx + 1u) & h->mask;
            }
        }
    }
    return true;
}

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13u;
    x ^= x >> 17u;
    x ^= x << 5u;
    *state = x;
    return x;
}

static double rand_unit(uint32_t *state)
{
    return (double)(xorshift32(state) >> 8u) * (1.0 / 16777216.0);
}

static bool cdt_seed_bridson_interior(cdt_mesh_t *m, double r_disk_in, double min_x, double min_y,
                                      double max_x, double max_y)
{
    uint32_t v_cur = m->v_count - 3u;
    if (v_cur >= m->max_non_super_verts) {
        return true;
    }
    uint32_t avail = m->max_non_super_verts - v_cur;
    uint32_t refine_reserve = (uint32_t)(0.40 * (double)m->max_non_super_verts);
    if (avail <= refine_reserve) {
        return true;
    }
    uint32_t bridson_cap = avail - refine_reserve;
    if (bridson_cap > (uint32_t)(0.60 * (double)avail)) {
        bridson_cap = (uint32_t)(0.60 * (double)avail);
    }
    if (bridson_cap == 0u) {
        return true;
    }

    double domain_area = 0.0;
    uint32_t seg_count = 0u;
    for (uint32_t tid = 0u; tid < m->t_count; tid++) {
        const cdt_tri_t *tr = &m->tris[tid];
        if ((tr->flags & (TRI_ALIVE | TRI_INTERIOR)) != (TRI_ALIVE | TRI_INTERIOR)) {
            continue;
        }
        we_cdt_vec2_t p0 = cdt_vp(m, tr->v[0]);
        we_cdt_vec2_t p1 = cdt_vp(m, tr->v[1]);
        we_cdt_vec2_t p2 = cdt_vp(m, tr->v[2]);
        domain_area += 0.5 * ((p1.x - p0.x) * (p2.y - p0.y) - (p1.y - p0.y) * (p2.x - p0.x));
        for (int e = 0; e < 3; e++) {
            if ((tr->constrained >> e) & 1u) {
                seg_count++;
            }
        }
    }
    if (domain_area <= 1e-6 || seg_count == 0u) {
        return true;
    }

    double r_disk = fmax(r_disk_in, 2.0 * sqrt(m->min_edge_len2));
    double est_needed_r = sqrt(domain_area / (0.85 * (double)bridson_cap));
    if (est_needed_r > r_disk) {
        r_disk = est_needed_r;
    }
    double clear_dist = 0.65 * r_disk;
    double clear_dist2 = clear_dist * clear_dist;
    double r2 = r_disk * r_disk;

    /* Collect constrained segments into a uniform spatial grid for O(1) clearance check */
    cdt_seg_pair_t *segs = malloc(seg_count * sizeof(cdt_seg_pair_t));
    if (!segs) {
        return false;
    }
    uint32_t s_idx = 0u;
    for (uint32_t tid = 0u; tid < m->t_count; tid++) {
        const cdt_tri_t *tr = &m->tris[tid];
        if ((tr->flags & (TRI_ALIVE | TRI_INTERIOR)) != (TRI_ALIVE | TRI_INTERIOR)) {
            continue;
        }
        for (int e = 0; e < 3; e++) {
            if ((tr->constrained >> e) & 1u) {
                segs[s_idx++] = (cdt_seg_pair_t){tr->v[e], tr->v[(e + 1) % 3]};
            }
        }
    }

    double span_x = fmax(max_x - min_x, 1.0);
    double span_y = fmax(max_y - min_y, 1.0);
    double seg_cell = fmax(clear_dist, fmax(span_x, span_y) / 64.0);
    uint32_t sgx = (uint32_t)ceil(span_x / seg_cell);
    uint32_t sgy = (uint32_t)ceil(span_y / seg_cell);
    if (sgx < 1u) {
        sgx = 1u;
    } else if (sgx > 64u) {
        sgx = 64u;
    }
    if (sgy < 1u) {
        sgy = 1u;
    } else if (sgy > 64u) {
        sgy = 64u;
    }

    rdp_spatial_grid_t sgrid;
    memset(&sgrid, 0, sizeof(sgrid));
    sgrid.gx = sgx;
    sgrid.gy = sgy;
    sgrid.min_x = min_x - 1e-6;
    sgrid.min_y = min_y - 1e-6;
    sgrid.inv_cell_w = (double)sgx / (span_x + 2e-6);
    sgrid.inv_cell_h = (double)sgy / (span_y + 2e-6);
    sgrid.seg_heads = malloc((size_t)sgx * sgy * sizeof(int32_t));
    sgrid.stamp = calloc(seg_count, sizeof(uint32_t));
    if (!sgrid.seg_heads || !sgrid.stamp) {
        rdp_grid_free(&sgrid);
        free(segs);
        return false;
    }
    for (size_t i = 0u; i < (size_t)sgx * sgy; i++) {
        sgrid.seg_heads[i] = -1;
    }
    for (uint32_t i = 0u; i < seg_count; i++) {
        if (!rdp_grid_insert_seg(&sgrid, i, cdt_vp(m, segs[i].va), cdt_vp(m, segs[i].vb))) {
            rdp_grid_free(&sgrid);
            free(segs);
            return false;
        }
    }

    uint32_t hash_cap = 1024u;
    while (hash_cap < (bridson_cap + v_cur) * 4u) {
        hash_cap *= 2u;
    }
    bridson_hash_t bhash;
    bhash.slots = calloc(hash_cap, sizeof(bridson_slot_t));
    bhash.mask = hash_cap - 1u;
    bhash.inv_cell = sqrt(2.0) / r_disk;

    int32_t *active = malloc(bridson_cap * sizeof(int32_t));
    if (!bhash.slots || !active) {
        free(active);
        free(bhash.slots);
        rdp_grid_free(&sgrid);
        free(segs);
        return false;
    }

    /* Seed existing interior pins so Bridson keeps r_disk clearance from them */
    for (uint32_t vi = 3u; vi < m->v_count; vi++) {
        if (m->verts[vi].is_pin && !m->verts[vi].is_boundary) {
            bridson_hash_insert(&bhash, m->verts[vi].x, m->verts[vi].y, (int32_t)vi);
        }
    }

    uint32_t active_len = 0u;
    uint32_t seeded_count = 0u;
    uint32_t rng = 0x9E3779B9u ^ seg_count;
    int32_t last_tri_hint = 0;

    bool ok = true;
    for (uint32_t tid = 0u;
         tid < m->t_count && seeded_count < bridson_cap && !m->hit_vertex_budget && ok; tid++) {
        const cdt_tri_t *tr = &m->tris[tid];
        if ((tr->flags & (TRI_ALIVE | TRI_INTERIOR)) != (TRI_ALIVE | TRI_INTERIOR)) {
            continue;
        }
        we_cdt_vec2_t p0 = cdt_vp(m, tr->v[0]);
        we_cdt_vec2_t p1 = cdt_vp(m, tr->v[1]);
        we_cdt_vec2_t p2 = cdt_vp(m, tr->v[2]);
        we_cdt_vec2_t cand = {(p0.x + p1.x + p2.x) / 3.0, (p0.y + p1.y + p2.y) / 3.0};

        if (!bridson_hash_far_enough(&bhash, m, cand.x, cand.y, r2)) {
            continue;
        }

        uint32_t ix0, iy0, ix1, iy1;
        rdp_grid_cell_range(&sgrid, cand.x - clear_dist, cand.y - clear_dist, cand.x + clear_dist,
                            cand.y + clear_dist, &ix0, &iy0, &ix1, &iy1);
        uint32_t st = ++sgrid.cur_stamp;
        if (st == 0u) {
            memset(sgrid.stamp, 0, seg_count * sizeof(uint32_t));
            st = ++sgrid.cur_stamp;
        }
        bool clear_ok = true;
        for (uint32_t cy = iy0; cy <= iy1 && clear_ok; cy++) {
            for (uint32_t cx = ix0; cx <= ix1 && clear_ok; cx++) {
                for (int32_t ni = sgrid.seg_heads[cy * sgrid.gx + cx]; ni >= 0;
                     ni = sgrid.pool[ni].next) {
                    uint32_t sid = sgrid.pool[ni].id;
                    if (sgrid.stamp[sid] == st) {
                        continue;
                    }
                    sgrid.stamp[sid] = st;
                    if (dist2_point_seg(cand, cdt_vp(m, segs[sid].va), cdt_vp(m, segs[sid].vb)) <
                        clear_dist2) {
                        clear_ok = false;
                        break;
                    }
                }
            }
        }
        if (!clear_ok) {
            continue;
        }

        bool inserted = false;
        int32_t vid = cdt_insert_point(m, cand, (int32_t)tid, false, false, false, &inserted);
        if (vid < 0) {
            if (m->hit_vertex_budget) {
                break;
            }
            ok = false;
            break;
        }
        if (!inserted) {
            continue;
        }
        last_tri_hint = m->verts[vid].any_tri;
        bridson_hash_insert(&bhash, cand.x, cand.y, vid);
        active[active_len++] = vid;
        seeded_count++;

        /* Expand Bridson disk around active list for this component */
        while (active_len > 0u && seeded_count < bridson_cap && !m->hit_vertex_budget && ok) {
            uint32_t pick = xorshift32(&rng) % active_len;
            int32_t base_v = active[pick];
            we_cdt_vec2_t bp = cdt_vp(m, base_v);
            bool found_one = false;

            for (int attempt = 0; attempt < 24; attempt++) {
                double ang = rand_unit(&rng) * (2.0 * M_PI);
                double rad = r_disk * sqrt(1.0 + 3.0 * rand_unit(&rng));
                we_cdt_vec2_t nc = {bp.x + rad * cos(ang), bp.y + rad * sin(ang)};
                if (nc.x <= min_x || nc.x >= max_x || nc.y <= min_y || nc.y >= max_y) {
                    continue;
                }
                if (!bridson_hash_far_enough(&bhash, m, nc.x, nc.y, r2)) {
                    continue;
                }

                uint32_t cx0, cy0, cx1, cy1;
                rdp_grid_cell_range(&sgrid, nc.x - clear_dist, nc.y - clear_dist, nc.x + clear_dist,
                                    nc.y + clear_dist, &cx0, &cy0, &cx1, &cy1);
                uint32_t cst = ++sgrid.cur_stamp;
                if (cst == 0u) {
                    memset(sgrid.stamp, 0, seg_count * sizeof(uint32_t));
                    cst = ++sgrid.cur_stamp;
                }
                bool seg_ok = true;
                for (uint32_t cy = cy0; cy <= cy1 && seg_ok; cy++) {
                    for (uint32_t cx = cx0; cx <= cx1 && seg_ok; cx++) {
                        for (int32_t ni = sgrid.seg_heads[cy * sgrid.gx + cx]; ni >= 0;
                             ni = sgrid.pool[ni].next) {
                            uint32_t sid = sgrid.pool[ni].id;
                            if (sgrid.stamp[sid] == cst) {
                                continue;
                            }
                            sgrid.stamp[sid] = cst;
                            if (dist2_point_seg(nc, cdt_vp(m, segs[sid].va),
                                                cdt_vp(m, segs[sid].vb)) < clear_dist2) {
                                seg_ok = false;
                                break;
                            }
                        }
                    }
                }
                if (!seg_ok) {
                    continue;
                }

                int on_edge = -1;
                int32_t dup_v = -1;
                int32_t loc_t = cdt_locate_point(m, last_tri_hint, nc, &on_edge, &dup_v);
                if (loc_t < 0 || dup_v >= 0 || !(m->tris[loc_t].flags & TRI_INTERIOR)) {
                    continue;
                }

                bool ins = false;
                int32_t nvid = cdt_insert_point(m, nc, loc_t, false, false, false, &ins);
                if (nvid < 0) {
                    if (m->hit_vertex_budget) {
                        break;
                    }
                    ok = false;
                    break;
                }
                if (ins) {
                    last_tri_hint = m->verts[nvid].any_tri;
                    bridson_hash_insert(&bhash, nc.x, nc.y, nvid);
                    active[active_len++] = nvid;
                    seeded_count++;
                    found_one = true;
                    break;
                }
            }
            if (!found_one) {
                active[pick] = active[--active_len];
            }
        }
    }

    free(active);
    free(bhash.slots);
    rdp_grid_free(&sgrid);
    free(segs);
    return ok;
}

/* =========================================================================
 * Chew's 2nd / Ruppert Quality Refinement
 * ========================================================================= */

static we_cdt_vec2_t tri_circumcenter(we_cdt_vec2_t a, we_cdt_vec2_t b, we_cdt_vec2_t c)
{
    double ax = a.x - c.x;
    double ay = a.y - c.y;
    double bx = b.x - c.x;
    double by = b.y - c.y;
    double a2 = ax * ax + ay * ay;
    double b2 = bx * bx + by * by;
    double d = 2.0 * (ax * by - ay * bx);
    double max_ab2 = fmax(a2, b2);
    if (!isfinite(d) || !isfinite(a2) || !isfinite(b2) || fabs(d) <= 1e-18 * fmax(max_ab2, 1.0)) {
        return (we_cdt_vec2_t){(a.x + b.x + c.x) / 3.0, (a.y + b.y + c.y) / 3.0};
    }
    double ux = (a2 * by - b2 * ay) / d;
    double uy = (ax * b2 - bx * a2) / d;
    double cx = c.x + ux;
    double cy = c.y + uy;
    if (!isfinite(cx) || !isfinite(cy) || fabs(cx) > 1e12 || fabs(cy) > 1e12) {
        return (we_cdt_vec2_t){(a.x + b.x + c.x) / 3.0, (a.y + b.y + c.y) / 3.0};
    }
    return (we_cdt_vec2_t){cx, cy};
}

static bool point_encroaches_edge(we_cdt_vec2_t p, we_cdt_vec2_t a, we_cdt_vec2_t b)
{
    return ((a.x - p.x) * (b.x - p.x) + (a.y - p.y) * (b.y - p.y)) <= 0.0;
}

typedef struct {
    cdt_seg_pair_t *items;
    uint32_t len;
    uint32_t cap;
} enc_queue_t;

static bool enc_queue_push(enc_queue_t *q, int32_t a, int32_t b)
{
    if (q->len == q->cap) {
        if (q->cap > UINT32_MAX / 2u) {
            return false;
        }
        uint32_t nc = (q->cap == 0u) ? 128u : (q->cap * 2u);
        cdt_seg_pair_t *ni = realloc(q->items, (size_t)nc * sizeof(cdt_seg_pair_t));
        if (!ni) {
            return false;
        }
        q->items = ni;
        q->cap = nc;
    }
    q->items[q->len++] = (cdt_seg_pair_t){a, b};
    return true;
}

static bool check_tri_edges_encroached(const cdt_mesh_t *m, int32_t tid, enc_queue_t *q)
{
    const cdt_tri_t *tr = &m->tris[tid];
    if ((tr->flags & (TRI_ALIVE | TRI_INTERIOR)) != (TRI_ALIVE | TRI_INTERIOR)) {
        return true;
    }
    for (int e = 0; e < 3; e++) {
        if ((tr->constrained >> e) & 1u) {
            int32_t a = tr->v[e];
            int32_t b = tr->v[(e + 1) % 3];
            int32_t opp = tr->v[(e + 2) % 3];
            we_cdt_vec2_t pa = cdt_vp(m, a);
            we_cdt_vec2_t pb = cdt_vp(m, b);
            if (dist2_pts(pa, pb) > m->min_edge_len2 &&
                point_encroaches_edge(cdt_vp(m, opp), pa, pb)) {
                if (!enc_queue_push(q, a, b)) {
                    return false;
                }
            }
        }
    }
    return true;
}

static bool cdt_split_constrained_segment(cdt_mesh_t *m, int32_t va, int32_t vb, enc_queue_t *q,
                                          bool *out_split)
{
    *out_split = false;
    if (m->v_count - 3u >= m->max_non_super_verts) {
        m->hit_vertex_budget = true;
        return false;
    }
    int32_t t = -1;
    int e = -1;
    if (!cdt_find_edge_between(m, va, vb, &t, &e)) {
        return true;
    }
    if (!((m->tris[t].constrained >> e) & 1u)) {
        return true;
    }

    we_cdt_vec2_t pa = cdt_vp(m, va);
    we_cdt_vec2_t pb = cdt_vp(m, vb);
    double len2 = dist2_pts(pa, pb);
    if (len2 <= m->min_edge_len2) {
        return true;
    }
    double len = sqrt(len2);

    /* Concentric power-of-2 shell split at acute input corners (< 60 deg) */
    double t_split = 0.5;
    bool a_acute = m->verts[va].has_acute_corner;
    bool b_acute = m->verts[vb].has_acute_corner;
    if (a_acute && !b_acute) {
        double d_shell = pow(2.0, floor(log2(0.5 * len)));
        t_split = clamp_f64(d_shell / len, 0.25, 0.5);
    } else if (!a_acute && b_acute) {
        double d_shell = pow(2.0, floor(log2(0.5 * len)));
        t_split = 1.0 - clamp_f64(d_shell / len, 0.25, 0.5);
    }

    we_cdt_vec2_t mid = {pa.x + t_split * (pb.x - pa.x), pa.y + t_split * (pb.y - pa.y)};
    if (dist2_pts(pa, mid) <= m->min_edge_len2 || dist2_pts(pb, mid) <= m->min_edge_len2) {
        return true;
    }
    int32_t vid = (int32_t)m->v_count++;
    m->verts[vid].x = mid.x;
    m->verts[vid].y = mid.y;
    m->verts[vid].is_super = false;
    m->verts[vid].is_boundary = true;
    m->verts[vid].is_pin = false;
    m->verts[vid].has_acute_corner = false;
    m->verts[vid].any_tri = t;

    int32_t new_tris[4] = {-1, -1, -1, -1};
    if (!cdt_split_edge_2to4(m, t, e, vid, true, new_tris)) {
        return false;
    }
    *out_split = true;
    for (int k = 0; k < 4; k++) {
        if (new_tris[k] >= 0) {
            if (!check_tri_edges_encroached(m, new_tris[k], q)) {
                return false;
            }
        }
    }
    return true;
}

static bool cdt_find_cavity_encroachments(cdt_mesh_t *m, int32_t start_t, we_cdt_vec2_t cc,
                                          enc_queue_t *q, bool *out_hit_blocking_edge)
{
    *out_hit_blocking_edge = false;

    /* Walk along the exact ray from start_t centroid toward cc to find the exit edge. */
    we_cdt_vec2_t p0 = cdt_vp(m, m->tris[start_t].v[0]);
    we_cdt_vec2_t p1 = cdt_vp(m, m->tris[start_t].v[1]);
    we_cdt_vec2_t p2 = cdt_vp(m, m->tris[start_t].v[2]);
    we_cdt_vec2_t c_start = {(p0.x + p1.x + p2.x) / 3.0, (p0.y + p1.y + p2.y) / 3.0};

    int32_t cur = start_t;
    int32_t cc_tri = -1;
    uint32_t walk_guard = 0u;
    int32_t stamp = ++m->cur_stamp;

    while (walk_guard++ < m->t_count + 8u) {
        m->tri_visit_stamp[cur] = stamp;
        const cdt_tri_t *tr = &m->tris[cur];
        int step_e = -1;
        int fallback_e = -1;
        bool inside_all = true;
        for (int e = 0; e < 3; e++) {
            we_cdt_vec2_t a = cdt_vp(m, tr->v[e]);
            we_cdt_vec2_t b = cdt_vp(m, tr->v[(e + 1) % 3]);
            if (orient2d(a, b, cc) < 0.0) {
                inside_all = false;
                if (fallback_e < 0) {
                    fallback_e = e;
                }
                double oa = orient2d(c_start, cc, a);
                double ob = orient2d(c_start, cc, b);
                if (oa <= 0.0 && ob >= 0.0) {
                    if (oa < 0.0 && ob > 0.0) {
                        step_e = e;
                        break;
                    }
                    /* Prefer an unvisited interior non-constrained edge when ray grazes a vertex.
                     */
                    bool is_con = ((tr->constrained >> e) & 1u) != 0u;
                    int32_t nb = tr->adj[e];
                    bool can_walk = !is_con && nb >= 0 && (m->tris[nb].flags & TRI_INTERIOR) &&
                                    m->tri_visit_stamp[nb] != stamp;
                    if (step_e < 0 || can_walk) {
                        step_e = e;
                    }
                }
            }
        }
        if (inside_all) {
            cc_tri = cur;
            break;
        }
        if (step_e < 0) {
            step_e = fallback_e;
        }
        if ((tr->constrained >> step_e) & 1u) {
            int32_t ea = tr->v[step_e];
            int32_t eb = tr->v[(step_e + 1) % 3];
            if (dist2_pts(cdt_vp(m, ea), cdt_vp(m, eb)) > m->min_edge_len2) {
                if (!enc_queue_push(q, ea, eb)) {
                    return false;
                }
            }
            *out_hit_blocking_edge = true;
            return true;
        }
        int32_t next = tr->adj[step_e];
        if (next < 0 || !(m->tris[next].flags & TRI_INTERIOR) ||
            m->tri_visit_stamp[next] == stamp) {
            *out_hit_blocking_edge = true;
            return true;
        }
        cur = next;
    }

    if (cc_tri < 0) {
        *out_hit_blocking_edge = true;
        return true;
    }

    /* Read-only cavity BFS around cc_tri to collect all encroached constrained edges */
    uint32_t bfs_cap = 64u;
    uint32_t bfs_head = 0u;
    uint32_t bfs_tail = 0u;
    int32_t *bfs = malloc(bfs_cap * sizeof(int32_t));
    if (!bfs) {
        return false;
    }
    int32_t cav_stamp = ++m->cur_stamp;
    m->tri_visit_stamp[cc_tri] = cav_stamp;
    bfs[bfs_tail++] = cc_tri;

    bool found_enc = false;
    while (bfs_head < bfs_tail) {
        int32_t ct = bfs[bfs_head++];
        const cdt_tri_t *ctr = &m->tris[ct];
        for (int e = 0; e < 3; e++) {
            if ((ctr->constrained >> e) & 1u) {
                int32_t a = ctr->v[e];
                int32_t b = ctr->v[(e + 1) % 3];
                we_cdt_vec2_t pa = cdt_vp(m, a);
                we_cdt_vec2_t pb = cdt_vp(m, b);
                if (point_encroaches_edge(cc, pa, pb)) {
                    if (dist2_pts(pa, pb) > m->min_edge_len2) {
                        if (!enc_queue_push(q, a, b)) {
                            free(bfs);
                            return false;
                        }
                    }
                    found_enc = true;
                }
                continue;
            }
            int32_t tn = ctr->adj[e];
            if (tn < 0 || !(m->tris[tn].flags & TRI_INTERIOR) ||
                m->tri_visit_stamp[tn] == cav_stamp) {
                continue;
            }
            if (incircle(cdt_vp(m, m->tris[tn].v[0]), cdt_vp(m, m->tris[tn].v[1]),
                         cdt_vp(m, m->tris[tn].v[2]), cc) > 0.0) {
                m->tri_visit_stamp[tn] = cav_stamp;
                if (bfs_tail == bfs_cap) {
                    bfs_cap *= 2u;
                    int32_t *nb = realloc(bfs, bfs_cap * sizeof(int32_t));
                    if (!nb) {
                        free(bfs);
                        return false;
                    }
                    bfs = nb;
                }
                bfs[bfs_tail++] = tn;
            }
        }
    }
    free(bfs);
    *out_hit_blocking_edge = found_enc;
    return true;
}

static bool cdt_refine_chew_ruppert(cdt_mesh_t *m)
{
    enc_queue_t enc_q = {0};

    for (uint32_t tid = 0u; tid < m->t_count; tid++) {
        if ((m->tris[tid].flags & (TRI_ALIVE | TRI_INTERIOR)) == (TRI_ALIVE | TRI_INTERIOR)) {
            if (!check_tri_edges_encroached(m, (int32_t)tid, &enc_q) ||
                !cdt_heap_update_tri(m, (int32_t)tid)) {
                free(enc_q.items);
                return false;
            }
        }
    }

    uint32_t iter_cap = m->max_non_super_verts * 4u + 256u;
    uint32_t iters = 0u;
    bool ok = true;
    int32_t pending_tri = -1;
    bool pending_did_split = false;

    while ((enc_q.len > 0u || m->heap_len > 0u) && iters++ < iter_cap) {
        if (m->v_count - 3u >= m->max_non_super_verts) {
            m->hit_vertex_budget = true;
            break;
        }

        if (enc_q.len > 0u) {
            cdt_seg_pair_t seg = enc_q.items[--enc_q.len];
            bool did_split = false;
            if (!cdt_split_constrained_segment(m, seg.va, seg.vb, &enc_q, &did_split)) {
                if (m->hit_vertex_budget) {
                    break;
                }
                ok = false;
                break;
            }
            if (did_split) {
                pending_did_split = true;
            }
            continue;
        }

        if (pending_tri >= 0) {
            if (!pending_did_split && (uint32_t)pending_tri < m->t_count &&
                (m->tris[pending_tri].flags & TRI_ALIVE) && m->tris[pending_tri].heap_idx >= 0) {
                cdt_heap_remove(m, pending_tri);
            }
            pending_tri = -1;
            pending_did_split = false;
            if (m->heap_len == 0u) {
                break;
            }
        }

        int32_t worst_t = m->heap[0];
        const cdt_tri_t *wt = &m->tris[worst_t];
        we_cdt_vec2_t cc =
            tri_circumcenter(cdt_vp(m, wt->v[0]), cdt_vp(m, wt->v[1]), cdt_vp(m, wt->v[2]));
        if (!isfinite(cc.x) || !isfinite(cc.y)) {
            cdt_heap_remove(m, worst_t);
            continue;
        }

        bool encroached = false;
        uint32_t enc_len_before = enc_q.len;
        if (!cdt_find_cavity_encroachments(m, worst_t, cc, &enc_q, &encroached)) {
            ok = false;
            break;
        }
        if (encroached) {
            if (enc_q.len == enc_len_before) {
                /* All blocking/encroached segments are below min_edge_len2; drop triangle. */
                cdt_heap_remove(m, worst_t);
            } else {
                pending_tri = worst_t;
                pending_did_split = false;
            }
            continue;
        }

        /* Verify circumcenter doesn't collide with an existing vertex */
        int on_edge = -1;
        int32_t dup_v = -1;
        int32_t loc_t = cdt_locate_point(m, worst_t, cc, &on_edge, &dup_v);
        if (loc_t < 0 || dup_v >= 0 || !(m->tris[loc_t].flags & TRI_INTERIOR)) {
            cdt_heap_remove(m, worst_t);
            continue;
        }

        bool inserted = false;
        int32_t vid = cdt_insert_point(m, cc, loc_t, false, false, true, &inserted);
        if (vid < 0) {
            if (m->hit_vertex_budget) {
                break;
            }
            cdt_heap_remove(m, worst_t);
            continue;
        }
        if (!inserted) {
            cdt_heap_remove(m, worst_t);
            continue;
        }

        /* Check if the new circumcenter encroaches any constrained edge of its fan */
        int32_t start_t = m->verts[vid].any_tri;
        int32_t fan_t = start_t;
        uint32_t fan_guard = 0u;
        while (fan_t >= 0 && fan_guard++ < m->t_count + 4u) {
            const cdt_tri_t *ftr = &m->tris[fan_t];
            int k = -1;
            for (int j = 0; j < 3; j++) {
                if (ftr->v[j] == vid) {
                    k = j;
                    break;
                }
            }
            if (k < 0) {
                break;
            }
            int opp_e = (k + 1) % 3;
            if ((ftr->constrained >> opp_e) & 1u) {
                int32_t ea = ftr->v[opp_e];
                int32_t eb = ftr->v[(opp_e + 1) % 3];
                we_cdt_vec2_t pa = cdt_vp(m, ea);
                we_cdt_vec2_t pb = cdt_vp(m, eb);
                if (dist2_pts(pa, pb) > m->min_edge_len2 && point_encroaches_edge(cc, pa, pb)) {
                    if (!enc_queue_push(&enc_q, ea, eb)) {
                        ok = false;
                        break;
                    }
                }
            }
            int32_t next_t = ftr->adj[k];
            if (next_t == start_t) {
                break;
            }
            fan_t = next_t;
        }
        if (!ok) {
            break;
        }
    }

    free(enc_q.items);
    return ok;
}

/* =========================================================================
 * Public Mesh Generation Entrypoints
 * ========================================================================= */

static uint32_t loops_bounds_and_count(const we_cdt_loop_t *loops, uint32_t count,
                                       double *out_min_x, double *out_min_y, double *out_max_x,
                                       double *out_max_y)
{
    uint32_t total = 0u;
    double min_x = DBL_MAX;
    double min_y = DBL_MAX;
    double max_x = -DBL_MAX;
    double max_y = -DBL_MAX;
    for (uint32_t m = 0u; m < count; m++) {
        total += loops[m].point_count;
        for (uint32_t k = 0u; k < loops[m].point_count; k++) {
            we_cdt_vec2_t p = loops[m].points[k];
            min_x = fmin(min_x, p.x);
            min_y = fmin(min_y, p.y);
            max_x = fmax(max_x, p.x);
            max_y = fmax(max_y, p.y);
        }
    }
    *out_min_x = min_x;
    *out_min_y = min_y;
    *out_max_x = max_x;
    *out_max_y = max_y;
    return total;
}

bool we_mesh_gen_from_loops(const we_cdt_loop_t *loops, uint32_t loop_count, uint32_t tex_width,
                            uint32_t tex_height, const we_mesh_gen_cfg_t *cfg,
                            const we_cdt_pin_t *pins, uint32_t pin_count,
                            we_mesh_gen_result_t *out_result)
{
    if (out_result) {
        memset(out_result, 0, sizeof(*out_result));
    }
    if (!loops || loop_count == 0u || !out_result) {
        return false;
    }
    if (tex_width == 0u || tex_height == 0u || tex_width > WE_CDT_MAX_IMAGE_DIM ||
        tex_height > WE_CDT_MAX_IMAGE_DIM) {
        return false;
    }
    if (pin_count > 0u && !pins) {
        return false;
    }

    we_mesh_gen_cfg_t s_cfg;
    if (!sanitize_cfg(cfg, &s_cfg)) {
        return false;
    }

    we_cdt_loop_t *simp_loops = NULL;
    uint32_t simp_count = 0u;
    if (!tp_rdp_simplify_loops(loops, loop_count, (double)s_cfg.simplify_eps_px, &simp_loops,
                               &simp_count)) {
        return false;
    }

    double min_x = 0.0;
    double min_y = 0.0;
    double max_x = 0.0;
    double max_y = 0.0;
    uint32_t b_vert_total =
        loops_bounds_and_count(simp_loops, simp_count, &min_x, &min_y, &max_x, &max_y);

    bool hit_budget_simplify = false;
    if (b_vert_total > s_cfg.max_vertices) {
        double span_d = fmax(fmax(max_x - min_x, max_y - min_y), 1.0);
        double lo_eps = (double)s_cfg.simplify_eps_px;
        double hi_eps = lo_eps;
        double max_eps = fmax(span_d * 2.0, 64.0);
        we_cdt_loop_t *best_loops = NULL;
        uint32_t best_count = 0u;

        for (int iter = 0; iter < 12 && hi_eps < max_eps; iter++) {
            hi_eps = fmin(hi_eps * 2.0, max_eps);
            we_cdt_loop_t *cand_loops = NULL;
            uint32_t cand_count = 0u;
            if (!tp_rdp_simplify_loops(loops, loop_count, hi_eps, &cand_loops, &cand_count)) {
                we_mesh_loops_free(best_loops, best_count);
                we_mesh_loops_free(simp_loops, simp_count);
                return false;
            }
            double cminx, cminy, cmaxx, cmaxy;
            uint32_t cand_verts =
                loops_bounds_and_count(cand_loops, cand_count, &cminx, &cminy, &cmaxx, &cmaxy);
            if (cand_verts <= s_cfg.max_vertices) {
                best_loops = cand_loops;
                best_count = cand_count;
                break;
            }
            we_mesh_loops_free(cand_loops, cand_count);
            lo_eps = hi_eps;
        }

        if (!best_loops) {
            we_mesh_loops_free(simp_loops, simp_count);
            return false;
        }

        for (int iter = 0; iter < 6 && (hi_eps - lo_eps) > 0.25; iter++) {
            double mid_eps = 0.5 * (lo_eps + hi_eps);
            we_cdt_loop_t *cand_loops = NULL;
            uint32_t cand_count = 0u;
            if (!tp_rdp_simplify_loops(loops, loop_count, mid_eps, &cand_loops, &cand_count)) {
                we_mesh_loops_free(best_loops, best_count);
                we_mesh_loops_free(simp_loops, simp_count);
                return false;
            }
            double cminx, cminy, cmaxx, cmaxy;
            uint32_t cand_verts =
                loops_bounds_and_count(cand_loops, cand_count, &cminx, &cminy, &cmaxx, &cmaxy);
            if (cand_verts <= s_cfg.max_vertices) {
                we_mesh_loops_free(best_loops, best_count);
                best_loops = cand_loops;
                best_count = cand_count;
                hi_eps = mid_eps;
            } else {
                we_mesh_loops_free(cand_loops, cand_count);
                lo_eps = mid_eps;
            }
        }

        we_mesh_loops_free(simp_loops, simp_count);
        simp_loops = best_loops;
        simp_count = best_count;
        b_vert_total =
            loops_bounds_and_count(simp_loops, simp_count, &min_x, &min_y, &max_x, &max_y);
        hit_budget_simplify = true;
    }

    cdt_mesh_t mesh;
    if (!cdt_mesh_init(&mesh, s_cfg.max_vertices, (double)s_cfg.min_angle_deg)) {
        we_mesh_loops_free(simp_loops, simp_count);
        return false;
    }
    mesh.hit_vertex_budget = hit_budget_simplify;

    double coord_scale = fmax(fmax((double)tex_width, (double)tex_height),
                              fmax(fmax(fabs(min_x), fabs(max_x)), fmax(fabs(min_y), fabs(max_y))));
    double min_clear = 4.0 * (double)FLT_EPSILON * coord_scale;
    mesh.min_edge_len2 = fmax(1e-6, min_clear * min_clear);
    mesh.dup_dist2 = mesh.min_edge_len2;

    /* Super-triangle enclosing all geometry by >= 100 * D */
    double cx = 0.5 * (min_x + max_x);
    double cy = 0.5 * (min_y + max_y);
    double D = fmax(fmax(max_x - min_x, max_y - min_y), 1.0);
    mesh.verts[0] = (cdt_vert_t){cx - 100.0 * D, cy - 100.0 * D, 0, true, false, false, false};
    mesh.verts[1] = (cdt_vert_t){cx + 300.0 * D, cy - 100.0 * D, 0, true, false, false, false};
    mesh.verts[2] = (cdt_vert_t){cx - 100.0 * D, cy + 300.0 * D, 0, true, false, false, false};
    mesh.v_count = 3u;

    int32_t root_t = cdt_alloc_tri(&mesh);
    if (root_t < 0) {
        cdt_mesh_free(&mesh);
        we_mesh_loops_free(simp_loops, simp_count);
        return false;
    }
    mesh.tris[root_t].v[0] = 0;
    mesh.tris[root_t].v[1] = 1;
    mesh.tris[root_t].v[2] = 2;

    int32_t *loop_vids = malloc((size_t)b_vert_total * sizeof(int32_t));
    if (!loop_vids) {
        cdt_mesh_free(&mesh);
        we_mesh_loops_free(simp_loops, simp_count);
        return false;
    }

    bool ok = true;
    int32_t hint_t = root_t;
    uint32_t lv_cursor = 0u;
    for (uint32_t m = 0u; m < simp_count && ok; m++) {
        uint32_t n = simp_loops[m].point_count;
        for (uint32_t k = 0u; k < n && ok; k++) {
            we_cdt_vec2_t p = simp_loops[m].points[k];
            int32_t vid = cdt_insert_point(&mesh, p, hint_t, true, true, false, NULL);
            if (vid < 0) {
                ok = false;
                break;
            }
            hint_t = mesh.verts[vid].any_tri;
            loop_vids[lv_cursor++] = vid;

            /* Mark acute corners (< 60 deg) for concentric shell splitting */
            we_cdt_vec2_t p_prev = simp_loops[m].points[(k > 0u) ? (k - 1u) : (n - 1u)];
            we_cdt_vec2_t p_next = simp_loops[m].points[(k + 1u < n) ? (k + 1u) : 0u];
            double u1x = p_prev.x - p.x;
            double u1y = p_prev.y - p.y;
            double u2x = p_next.x - p.x;
            double u2y = p_next.y - p.y;
            double l1 = hypot(u1x, u1y);
            double l2 = hypot(u2x, u2y);
            if (l1 > 1e-12 && l2 > 1e-12) {
                double cos_a = (u1x * u2x + u1y * u2y) / (l1 * l2);
                if (cos_a > 0.5) {
                    mesh.verts[vid].has_acute_corner = true;
                }
            }
        }
    }

    /* Recover all constrained boundary segments via Sloan visibility-walk + flip */
    uint32_t seg_off = 0u;
    for (uint32_t m = 0u; m < simp_count && ok; m++) {
        uint32_t n = simp_loops[m].point_count;
        for (uint32_t k = 0u; k < n && ok; k++) {
            int32_t va = loop_vids[seg_off + k];
            int32_t vb = loop_vids[seg_off + ((k + 1u < n) ? (k + 1u) : 0u)];
            if (!cdt_recover_segment_sloan(&mesh, va, vb)) {
                ok = false;
            }
        }
        seg_off += n;
    }

    free(loop_vids);
    we_mesh_loops_free(simp_loops, simp_count);

    if (ok) {
        ok = cdt_flood_fill_interior(&mesh);
    }

    /* Insert user pins inside the interior domain */
    for (uint32_t i = 0u; i < pin_count && ok; i++) {
        if ((pins[i].flags & 1u) == 0u) {
            continue;
        }
        we_cdt_vec2_t pp = {pins[i].px, pins[i].py};
        if (!isfinite(pp.x) || !isfinite(pp.y) || fabs(pp.x) > 1e9 || fabs(pp.y) > 1e9) {
            ok = false;
            break;
        }
        int on_edge = -1;
        int32_t dup_v = -1;
        int32_t loc_t = cdt_locate_point(&mesh, hint_t, pp, &on_edge, &dup_v);
        if (loc_t < 0) {
            continue;
        }
        if (dup_v >= 0) {
            mesh.verts[dup_v].is_pin = true;
            continue;
        }
        bool in_domain = (mesh.tris[loc_t].flags & TRI_INTERIOR) != 0u;
        if (!in_domain && on_edge >= 0 && mesh.tris[loc_t].adj[on_edge] >= 0) {
            in_domain = (mesh.tris[mesh.tris[loc_t].adj[on_edge]].flags & TRI_INTERIOR) != 0u;
        }
        if (!in_domain) {
            continue;
        }

        /* Snap near-edge pins onto the edge so float32 emission cannot form a zero-area sliver. */
        if (on_edge < 0) {
            int snap_e = -1;
            double best_d2 = mesh.min_edge_len2;
            for (int e = 0; e < 3; e++) {
                double d2 = dist2_point_seg(pp, cdt_vp(&mesh, mesh.tris[loc_t].v[e]),
                                            cdt_vp(&mesh, mesh.tris[loc_t].v[(e + 1) % 3]));
                if (d2 <= best_d2) {
                    best_d2 = d2;
                    snap_e = e;
                }
            }
            if (snap_e >= 0) {
                int32_t va = mesh.tris[loc_t].v[snap_e];
                int32_t vb = mesh.tris[loc_t].v[(snap_e + 1) % 3];
                we_cdt_vec2_t ea = cdt_vp(&mesh, va);
                we_cdt_vec2_t eb = cdt_vp(&mesh, vb);
                double vx = eb.x - ea.x;
                double vy = eb.y - ea.y;
                double l2 = vx * vx + vy * vy;
                double t_proj =
                    (l2 > 1e-24)
                        ? clamp_f64(((pp.x - ea.x) * vx + (pp.y - ea.y) * vy) / l2, 0.0, 1.0)
                        : 0.0;
                we_cdt_vec2_t proj = {ea.x + t_proj * vx, ea.y + t_proj * vy};
                if (dist2_pts(proj, ea) <= mesh.dup_dist2) {
                    mesh.verts[va].is_pin = true;
                    continue;
                }
                if (dist2_pts(proj, eb) <= mesh.dup_dist2) {
                    mesh.verts[vb].is_pin = true;
                    continue;
                }
                if (mesh.v_count - 3u >= mesh.max_non_super_verts) {
                    mesh.hit_vertex_budget = true;
                    break;
                }
                int32_t pvid = (int32_t)mesh.v_count++;
                mesh.verts[pvid].x = proj.x;
                mesh.verts[pvid].y = proj.y;
                mesh.verts[pvid].is_super = false;
                mesh.verts[pvid].is_boundary =
                    ((mesh.tris[loc_t].constrained >> snap_e) & 1u) != 0u;
                mesh.verts[pvid].is_pin = true;
                mesh.verts[pvid].has_acute_corner = false;
                mesh.verts[pvid].any_tri = loc_t;
                if (!cdt_split_edge_2to4(&mesh, loc_t, snap_e, pvid, false, NULL)) {
                    ok = false;
                    break;
                }
                hint_t = mesh.verts[pvid].any_tri;
                continue;
            }
        }

        bool pin_on_boundary =
            (on_edge >= 0 && ((mesh.tris[loc_t].constrained >> on_edge) & 1u) != 0u);
        int32_t pvid = cdt_insert_point(&mesh, pp, loc_t, pin_on_boundary, true, false, NULL);
        if (pvid < 0) {
            if (mesh.hit_vertex_budget) {
                break;
            }
            ok = false;
            break;
        }
        hint_t = mesh.verts[pvid].any_tri;
    }

    if (ok && !mesh.hit_vertex_budget) {
        ok = cdt_seed_bridson_interior(&mesh, (double)s_cfg.interior_spacing, min_x, min_y, max_x,
                                       max_y);
    }

    if (ok && !mesh.hit_vertex_budget) {
        ok = cdt_refine_chew_ruppert(&mesh);
    }

    if (!ok) {
        cdt_mesh_free(&mesh);
        return false;
    }

    /* Compact active interior vertices and emit CCW triangles */
    int32_t *remap = malloc((size_t)mesh.v_count * sizeof(int32_t));
    if (!remap) {
        cdt_mesh_free(&mesh);
        return false;
    }
    for (uint32_t i = 0u; i < mesh.v_count; i++) {
        remap[i] = -1;
    }

    uint32_t out_vcount = 0u;
    uint32_t out_tcount = 0u;
    for (uint32_t tid = 0u; tid < mesh.t_count; tid++) {
        const cdt_tri_t *tr = &mesh.tris[tid];
        if ((tr->flags & (TRI_ALIVE | TRI_INTERIOR)) != (TRI_ALIVE | TRI_INTERIOR)) {
            continue;
        }
        for (int k = 0; k < 3; k++) {
            int32_t v = tr->v[k];
            if (remap[v] < 0) {
                remap[v] = (int32_t)out_vcount++;
            }
        }
        out_tcount++;
    }

    if (out_vcount < 3u || out_vcount > WE_MDL_MAX_VERTICES || out_tcount == 0u ||
        out_tcount > WE_MDL_MAX_TRIANGLES) {
        free(remap);
        cdt_mesh_free(&mesh);
        return false;
    }

    we_mdl_vertex_t *out_verts = calloc(out_vcount, sizeof(we_mdl_vertex_t));
    uint16_t *out_indices = malloc((size_t)out_tcount * 3u * sizeof(uint16_t));
    if (!out_verts || !out_indices) {
        free(out_indices);
        free(out_verts);
        free(remap);
        cdt_mesh_free(&mesh);
        return false;
    }

    double tw = (double)tex_width;
    double th = (double)tex_height;
    for (uint32_t vi = 0u; vi < mesh.v_count; vi++) {
        if (remap[vi] < 0) {
            continue;
        }
        we_mdl_vertex_t *dst = &out_verts[remap[vi]];
        double px = mesh.verts[vi].x;
        double py = mesh.verts[vi].y;
        dst->pos[0] = (float)(px - 0.5 * tw);
        dst->pos[1] = (float)(0.5 * th - py);
        dst->pos[2] = 0.0f;
        dst->blend_indices[0] = 0u;
        dst->blend_indices[1] = 0u;
        dst->blend_indices[2] = 0u;
        dst->blend_indices[3] = 0u;
        dst->weight[0] = 1.0f;
        dst->weight[1] = 0.0f;
        dst->weight[2] = 0.0f;
        dst->weight[3] = 0.0f;
        dst->texcoord[0] = fminf(fmaxf((float)(px / tw), 0.0f), 1.0f);
        dst->texcoord[1] = fminf(fmaxf((float)(py / th), 0.0f), 1.0f);
        if (!isfinite(dst->pos[0]) || !isfinite(dst->pos[1]) || !isfinite(dst->texcoord[0]) ||
            !isfinite(dst->texcoord[1])) {
            free(out_indices);
            free(out_verts);
            free(remap);
            cdt_mesh_free(&mesh);
            return false;
        }
    }

    uint32_t idx_cur = 0u;
    double min_ang_deg = 180.0;
    double q_sum = 0.0;
    const double four_sqrt3 = 4.0 * sqrt(3.0);

    for (uint32_t tid = 0u; tid < mesh.t_count; tid++) {
        const cdt_tri_t *tr = &mesh.tris[tid];
        if ((tr->flags & (TRI_ALIVE | TRI_INTERIOR)) != (TRI_ALIVE | TRI_INTERIOR)) {
            continue;
        }
        int32_t v0 = tr->v[0];
        int32_t v1 = tr->v[1];
        int32_t v2 = tr->v[2];
        we_cdt_vec2_t p0 = cdt_vp(&mesh, v0);
        we_cdt_vec2_t p1 = cdt_vp(&mesh, v1);
        we_cdt_vec2_t p2 = cdt_vp(&mesh, v2);
        if (orient2d(p0, p1, p2) < 0.0) {
            int32_t tmp = v1;
            v1 = v2;
            v2 = tmp;
            we_cdt_vec2_t tp = p1;
            p1 = p2;
            p2 = tp;
        }

        uint16_t iv0 = (uint16_t)remap[v0];
        uint16_t iv1 = (uint16_t)remap[v1];
        uint16_t iv2 = (uint16_t)remap[v2];
        const we_mdl_vertex_t *fv0 = &out_verts[iv0];
        const we_mdl_vertex_t *fv1 = &out_verts[iv1];
        const we_mdl_vertex_t *fv2 = &out_verts[iv2];
        double f_area2 = ((double)fv1->pos[0] - (double)fv0->pos[0]) *
                             (-(double)fv2->pos[1] - (-(double)fv0->pos[1])) -
                         (-(double)fv1->pos[1] - (-(double)fv0->pos[1])) *
                             ((double)fv2->pos[0] - (double)fv0->pos[0]);
        if (!isfinite(f_area2) || f_area2 <= 0.0) {
            free(out_indices);
            free(out_verts);
            free(remap);
            cdt_mesh_free(&mesh);
            return false;
        }

        out_indices[idx_cur++] = iv0;
        out_indices[idx_cur++] = iv1;
        out_indices[idx_cur++] = iv2;

        double l0_2 = dist2_pts(p0, p1);
        double l1_2 = dist2_pts(p1, p2);
        double l2_2 = dist2_pts(p2, p0);
        double a = l0_2;
        double b = l1_2;
        double c = l2_2;
        if (a > b) {
            double t = a;
            a = b;
            b = t;
        }
        if (b > c) {
            double t = b;
            b = c;
            c = t;
        }
        if (a > b) {
            double t = a;
            a = b;
            b = t;
        }
        double denom = 2.0 * sqrt(b * c);
        double ang_deg = 0.0;
        if (denom > 1e-24) {
            double cos_t = clamp_f64((b + c - a) / denom, -1.0, 1.0);
            ang_deg = acos(cos_t) * (180.0 / M_PI);
        }
        if (ang_deg < min_ang_deg) {
            min_ang_deg = ang_deg;
        }

        double area2 = fabs((p1.x - p0.x) * (p2.y - p0.y) - (p1.y - p0.y) * (p2.x - p0.x));
        double perim_sq = l0_2 + l1_2 + l2_2;
        double q = (perim_sq > 1e-24) ? ((four_sqrt3 * 0.5 * area2) / perim_sq) : 0.0;
        q_sum += q;
    }

    out_result->vertices = out_verts;
    out_result->vertex_count = out_vcount;
    out_result->indices = out_indices;
    out_result->index_count = idx_cur;
    out_result->triangle_count = out_tcount;
    out_result->achieved_min_angle_deg = (float)min_ang_deg;
    out_result->achieved_mean_quality = (float)(q_sum / (double)out_tcount);
    out_result->hit_vertex_budget = mesh.hit_vertex_budget;

    free(remap);
    cdt_mesh_free(&mesh);
    return true;
}

bool we_mesh_gen_from_alpha(const uint8_t *alpha, uint32_t width, uint32_t height,
                            uint32_t stride_bytes, const we_mesh_gen_cfg_t *cfg,
                            const we_cdt_pin_t *pins, uint32_t pin_count,
                            we_mesh_gen_result_t *out_result)
{
    if (out_result) {
        memset(out_result, 0, sizeof(*out_result));
    }
    if (!alpha || !out_result) {
        return false;
    }

    we_cdt_loop_t *loops = NULL;
    uint32_t loop_count = 0u;
    if (!we_mesh_extract_contours(alpha, width, height, stride_bytes, cfg, &loops, &loop_count)) {
        return false;
    }
    if (loop_count == 0u) {
        we_mesh_loops_free(loops, loop_count);
        return false;
    }

    bool ok =
        we_mesh_gen_from_loops(loops, loop_count, width, height, cfg, pins, pin_count, out_result);
    we_mesh_loops_free(loops, loop_count);
    return ok;
}

bool we_mesh_gen_from_rgba8(const uint8_t *rgba8, uint32_t width, uint32_t height,
                            uint32_t stride_bytes, const we_mesh_gen_cfg_t *cfg,
                            const we_cdt_pin_t *pins, uint32_t pin_count,
                            we_mesh_gen_result_t *out_result)
{
    if (out_result) {
        memset(out_result, 0, sizeof(*out_result));
    }
    if (!rgba8 || !out_result) {
        return false;
    }
    if (width == 0u || height == 0u || width > WE_CDT_MAX_IMAGE_DIM ||
        height > WE_CDT_MAX_IMAGE_DIM) {
        return false;
    }
    if ((uint64_t)width * (uint64_t)height > (uint64_t)WE_CDT_MAX_IMAGE_PIXELS) {
        return false;
    }
    uint32_t min_stride = width * 4u;
    if (stride_bytes == 0u) {
        stride_bytes = min_stride;
    } else if (stride_bytes < min_stride) {
        return false;
    }

    size_t n_pix = (size_t)width * (size_t)height;
    uint8_t *alpha = malloc(n_pix);
    if (!alpha) {
        return false;
    }
    for (uint32_t y = 0u; y < height; y++) {
        const uint8_t *row = rgba8 + (size_t)y * stride_bytes;
        uint8_t *dst = alpha + (size_t)y * width;
        for (uint32_t x = 0u; x < width; x++) {
            dst[x] = row[x * 4u + 3u];
        }
    }

    bool ok = we_mesh_gen_from_alpha(alpha, width, height, width, cfg, pins, pin_count, out_result);
    free(alpha);
    return ok;
}

void we_mesh_gen_result_free(we_mesh_gen_result_t *result)
{
    if (!result) {
        return;
    }
    free(result->vertices);
    free(result->indices);
    memset(result, 0, sizeof(*result));
}
