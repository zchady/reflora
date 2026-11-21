// BC7 encoder (nvn_bc7_encode.h). Per block:
//   1. one subset: mode 6 (RGBA, 4-bit indices); with alpha also modes 5 and 4 (separate alpha indices, rotation 0);
//   2. unless that is already within kGoodEnough: two subsets, the 64 partitions ranked by how well a line fits each
//      subset (the residual of its principal axis), the best kTry2 tried in modes 1 and 3 (opaque) or 7 (alpha);
//   3. opaque blocks still above kGoodEnough: three subsets the same way, modes 2 and 0 (0 only has partitions 0-15).
// Each subset: endpoints from its principal axis, quantized for every p-bit choice, indices to the nearest palette
// entry, then two rounds of least-squares endpoint refinement. Error: squared differences of every channel; an opaque
// block's alpha weighs 64 times, so an opaque block stays exactly opaque. The sharp multi-colour edges of small icons,
// where single-subset or poorly partitioned encoders leave wrong-coloured blocks, are what steps 2 and 3 are for.
#include "nvn_bc7_encode.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nx::nvn {

namespace {

// nvn_bc.cpp's tables: bit i (2 subsets) or bits 2i..2i+1 (3 subsets) give pixel i's subset.
const uint16_t kPart2[64] = {
    0xcccc, 0x8888, 0xeeee, 0xecc8, 0xc880, 0xfeec, 0xfec8, 0xec80, 0xc800, 0xffec, 0xfe80, 0xe800, 0xffe8,
    0xff00, 0xfff0, 0xf000, 0xf710, 0x008e, 0x7100, 0x08ce, 0x008c, 0x7310, 0x3100, 0x8cce, 0x088c, 0x3110,
    0x6666, 0x366c, 0x17e8, 0x0ff0, 0x718e, 0x399c, 0xaaaa, 0xf0f0, 0x5a5a, 0x33cc, 0x3c3c, 0x55aa, 0x9696,
    0xa55a, 0x73ce, 0x13c8, 0x324c, 0x3bdc, 0x6996, 0xc33c, 0x9966, 0x0660, 0x0272, 0x04e4, 0x4e40, 0x2720,
    0xc936, 0x936c, 0x39c6, 0x639c, 0x9336, 0x9cc6, 0x817e, 0xe718, 0xccf0, 0x0fcc, 0x7744, 0xee22,
};
const uint32_t kPart3[64] = {
    0xaa685050, 0x6a5a5040, 0x5a5a4200, 0x5450a0a8, 0xa5a50000, 0xa0a05050, 0x5555a0a0, 0x5a5a5050, 0xaa550000,
    0xaa555500, 0xaaaa5500, 0x90909090, 0x94949494, 0xa4a4a4a4, 0xa9a59450, 0x2a0a4250, 0xa5945040, 0x0a425054,
    0xa5a5a500, 0x55a0a0a0, 0xa8a85454, 0x6a6a4040, 0xa4a45000, 0x1a1a0500, 0x0050a4a4, 0xaaa59090, 0x14696914,
    0x69691400, 0xa08585a0, 0xaa821414, 0x50a4a450, 0x6a5a0200, 0xa9a58000, 0x5090a0a8, 0xa8a09050, 0x24242424,
    0x00aa5500, 0x24924924, 0x24499224, 0x50a50a50, 0x500aa550, 0xaaaa4444, 0x66660000, 0xa5a0a5a0, 0x50a050a0,
    0x69286928, 0x44aaaa44, 0x66666600, 0xaa444444, 0x54a854a8, 0x95809580, 0x96969600, 0xa85454a8, 0x80959580,
    0xaa141414, 0x96960000, 0xaaaa1414, 0xa05050a0, 0xa0a5a5a0, 0x96000000, 0x40804080, 0xa9a8a9a8, 0xaaaaaa44,
    0x2a4a5254,
};
const uint8_t kAnchor2[64] = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,  8,  2,  2, 8,
                              8,  15, 2,  8,  2,  2,  8,  8,  2,  2,  15, 15, 6,  8,  2,  8,  15, 15, 2,  8,  2, 2,
                              2,  15, 15, 6,  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2,  15};
const uint8_t kAnchor3a[64] = {3, 3, 15, 15, 8, 3, 15, 15, 8,  8,  6,  6,  6,  5,  3, 3, 3,  3,  8,  15, 3, 3,
                               6, 10, 5, 8,  8, 6, 8,  5,  15, 15, 8,  15, 3,  5,  6, 10, 8, 15, 15, 3,  15, 5,
                               15, 15, 15, 15, 3, 15, 5, 5,  5,  8,  5,  10, 5,  10, 8, 13, 15, 12, 3,  3};
const uint8_t kAnchor3b[64] = {15, 8,  8,  3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,  15, 8,  15, 3,  15, 8,
                               15, 8,  3,  15, 6,  10, 15, 15, 10, 8,  15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15,
                               3,  6,  6,  8,  15, 3,  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8};
const uint8_t kW2[4] = {0, 21, 43, 64};
const uint8_t kW3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
const uint8_t kW4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
const uint8_t* weights(int bits) { return bits == 2 ? kW2 : bits == 3 ? kW3 : kW4; }

constexpr int kTry2 = 6, kTry3 = 4;
constexpr int kRankFine = 16;  // partitions given the line-residual score after the variance pre-ranking
// Squared error of a whole block (all channels) under which no partitioned mode is tried: an RMS of about 1.2
// levels per channel of an RGBA block.
constexpr float kGoodEnough = 96.0f;
// Squared error of a whole block above which every partition and every rotation is tried (rare: the blocks that would
// otherwise stay visibly wrong, an RMS of about 5 levels per RGBA channel).
constexpr float kRescue = 1600.0f;
constexpr float kOpaqueAlphaWeight = 65536.0f;  // an opaque block's alpha: exactly 255 (mode 6's p-bits)

int subset_of(int subsets, int part, int i) {
    if (subsets == 2) return kPart2[part] >> i & 1;
    if (subsets == 3) return kPart3[part] >> (2 * i) & 3;
    return 0;
}
int anchor_of(int subsets, int part, int s) {
    if (s == 0) return 0;
    if (subsets == 2) return kAnchor2[part];
    return s == 1 ? kAnchor3a[part] : kAnchor3b[part];
}

uint32_t expand(uint32_t v, int bits) {
    v <<= 8 - bits;
    return v | (v >> bits);
}

struct Writer {
    uint8_t b[16] = {};
    int pos = 0;
    void put(uint32_t v, int n) {
        for (int k = 0; k < n; k++, pos++)
            if (v >> k & 1) b[pos >> 3] |= uint8_t(1u << (pos & 7));
    }
};

// One subset's endpoints as stored: quantized channels (without p-bit) and p-bits.
struct Ends {
    uint8_t q[2][4] = {};
    uint8_t p[2] = {};
    uint8_t e[2][4] = {};  // expanded to 8 bits, for the palette
};

// The precision of one encoding attempt: colour bits, alpha bits (0: opaque mode, alpha 255), p-bit kind (0 none,
// 1 one per endpoint, 2 one per subset), index bits, and the channels the indices cover (3: RGB, 4: RGBA).
struct Prec {
    int cb, ab, pkind, ib, nc;
};

// Quantizes target endpoint values (0..255 floats) with p-bits p0/p1 under `pr`.
void quantize(const float t[2][4], int p0, int p1, const Prec& pr, Ends& out) {
    const int ps[2] = {p0, p1};
    for (int k = 0; k < 2; k++) {
        out.p[k] = uint8_t(ps[k]);
        for (int c = 0; c < 4; c++) {
            const int bits = c < 3 ? pr.cb : pr.ab;
            if (!bits) {  // opaque mode
                out.q[k][c] = 0, out.e[k][c] = 255;
                continue;
            }
            const bool hp = pr.pkind != 0;
            const int n = bits + (hp ? 1 : 0), maxq = (1 << bits) - 1;
            const float scaled = t[k][c] / 255.0f * float((1 << n) - 1);
            int q0 = hp ? int(std::lround((scaled - ps[k]) / 2.0f)) : int(std::lround(scaled));
            int best = 0, bestd = 1 << 30;
            for (int q = std::max(0, q0 - 1); q <= std::min(maxq, q0 + 1); q++) {
                const uint32_t code = hp ? uint32_t((q << 1) | ps[k]) : uint32_t(q);
                const int v = int(expand(code, n)), d = std::abs(v - int(std::lround(t[k][c])));
                if (d < bestd) bestd = d, best = q;
            }
            out.q[k][c] = uint8_t(best);
            out.e[k][c] = uint8_t(expand(hp ? uint32_t((best << 1) | ps[k]) : uint32_t(best), n));
        }
    }
}

// Indices of `n` pixels against the palette of `en` (channels [c0, c0 + nc)), returning the weighted squared error of
// those channels; `idx` receives each pixel's index.
float assign(const float (*x)[4], int n, const Ends& en, int ib, int c0, int nc, const float* w, uint8_t* idx) {
    const uint8_t* wt = weights(ib);
    const int levels = 1 << ib;
    float pal[16][4];
    for (int k = 0; k < levels; k++)
        for (int c = c0; c < c0 + nc; c++)
            pal[k][c] = float(((64 - wt[k]) * en.e[0][c] + wt[k] * en.e[1][c] + 32) >> 6);
    // The palette lies on the segment between the endpoints, its weights nearly evenly spaced: a pixel's projection on
    // the segment gives its index to within one, so only the entries around it are measured.
    float dir[4] = {}, len2 = 0;
    for (int c = c0; c < c0 + nc; c++) dir[c] = pal[levels - 1][c] - pal[0][c], len2 += dir[c] * dir[c] * w[c];
    float total = 0;
    for (int i = 0; i < n; i++) {
        int k0 = 0;
        if (len2 > 0) {
            float t = 0;
            for (int c = c0; c < c0 + nc; c++) t += (x[i][c] - pal[0][c]) * dir[c] * w[c];
            k0 = std::clamp(int(std::lround(t / len2 * float(levels - 1))), 0, levels - 1);
        }
        float best = 1e30f;
        int bi = 0;
        for (int k = std::max(0, k0 - 1); k <= std::min(levels - 1, k0 + 1); k++) {
            float d = 0;
            for (int c = c0; c < c0 + nc; c++) {
                const float v = pal[k][c] - x[i][c];
                d += v * v * w[c];
            }
            if (d < best) best = d, bi = k;
        }
        idx[i] = uint8_t(bi);
        total += best;
    }
    return total;
}

// Principal axis of `n` points in channels [c0, c0 + nc), and their mean.
void principal_axis(const float (*x)[4], int n, int c0, int nc, float mean[4], float axis[4]) {
    for (int c = 0; c < 4; c++) mean[c] = 0, axis[c] = 0;
    for (int i = 0; i < n; i++)
        for (int c = c0; c < c0 + nc; c++) mean[c] += x[i][c];
    for (int c = c0; c < c0 + nc; c++) mean[c] /= float(std::max(n, 1));
    float cov[4][4] = {};
    for (int i = 0; i < n; i++)
        for (int a = c0; a < c0 + nc; a++)
            for (int b = c0; b < c0 + nc; b++) cov[a][b] += (x[i][a] - mean[a]) * (x[i][b] - mean[b]);
    // start from the channel of largest variance (stable for grey and single-channel data)
    int big = c0;
    for (int c = c0; c < c0 + nc; c++)
        if (cov[c][c] > cov[big][big]) big = c;
    float v[4] = {};
    for (int c = c0; c < c0 + nc; c++) v[c] = cov[big][c];
    if (cov[big][big] <= 0) {
        for (int c = c0; c < c0 + nc; c++) axis[c] = 1.0f / std::sqrt(float(nc));
        return;
    }
    for (int it = 0; it < 8; it++) {
        float nv[4] = {};
        for (int a = c0; a < c0 + nc; a++)
            for (int b = c0; b < c0 + nc; b++) nv[a] += cov[a][b] * v[b];
        float len = 0;
        for (int c = c0; c < c0 + nc; c++) len += nv[c] * nv[c];
        len = std::sqrt(len);
        if (len <= 1e-12f) break;
        for (int c = c0; c < c0 + nc; c++) v[c] = nv[c] / len;
    }
    for (int c = c0; c < c0 + nc; c++) axis[c] = v[c];
}

// Fits one group of channels [c0, c0 + nc) of `n` pixels with `pr` (its alpha bits apply when the group holds
// alpha): best endpoints over every p-bit choice, with least-squares refinement. Returns the error; fills `en`
// (only channels of the group) and `idx`.
float fit(const float (*x)[4], int n, const Prec& pr, int ib, int c0, int nc, const float* w, Ends& en, uint8_t* idx,
          int rounds = 3) {
    float mean[4], axis[4];
    principal_axis(x, n, c0, nc, mean, axis);
    float tmin = 1e30f, tmax = -1e30f;
    for (int i = 0; i < n; i++) {
        float t = 0;
        for (int c = c0; c < c0 + nc; c++) t += (x[i][c] - mean[c]) * axis[c];
        tmin = std::min(tmin, t), tmax = std::max(tmax, t);
    }
    if (n == 0) tmin = tmax = 0;
    float target[2][4] = {};
    for (int c = 0; c < 4; c++) {
        target[0][c] = std::clamp(mean[c] + tmin * axis[c], 0.0f, 255.0f);
        target[1][c] = std::clamp(mean[c] + tmax * axis[c], 0.0f, 255.0f);
    }
    const int combos = pr.pkind == 1 ? 4 : pr.pkind == 2 ? 2 : 1;
    float best = 1e30f, prev = 1e30f;
    uint8_t tmp[16];
    for (int round = 0; round < rounds; round++) {
        const bool last = round == rounds - 1;
        // Before the last round, only the p-bit choice that quantizes the target endpoints most closely is measured;
        // the last round (and a round that stopped improving) measures every choice.
        int only = -1;
        if (!last && combos > 1) {
            float bestq = 1e30f;
            for (int k = 0; k < combos; k++) {
                const int p0 = pr.pkind == 1 ? (k & 1) : pr.pkind == 2 ? k : 0;
                const int p1 = pr.pkind == 1 ? (k >> 1) : p0;
                Ends e;
                quantize(target, p0, p1, pr, e);
                float d = 0;
                for (int s = 0; s < 2; s++)
                    for (int c = c0; c < c0 + nc; c++) {
                        const float v = float(e.e[s][c]) - target[s][c];
                        d += v * v * w[c];
                    }
                if (d < bestq) bestq = d, only = k;
            }
        }
        Ends round_best;
        uint8_t cur[16];
        float round_err = 1e30f;
        for (int k = 0; k < combos; k++) {
            if (only >= 0 && k != only) continue;
            const int p0 = pr.pkind == 1 ? (k & 1) : pr.pkind == 2 ? k : 0;
            const int p1 = pr.pkind == 1 ? (k >> 1) : p0;
            Ends e;
            quantize(target, p0, p1, pr, e);
            const float err = assign(x, n, e, ib, c0, nc, w, tmp);
            if (err < round_err) round_err = err, round_best = e, std::memcpy(cur, tmp, size_t(n));
            if (err < best) {
                best = err, std::memcpy(idx, tmp, size_t(n));
                for (int s = 0; s < 2; s++)
                    for (int c = c0; c < c0 + nc; c++) en.q[s][c] = e.q[s][c], en.e[s][c] = e.e[s][c];
                en.p[0] = e.p[0], en.p[1] = e.p[1];
            }
        }
        if (last || best == 0) break;
        if (round_err >= prev) rounds = round + 2;  // no better than the round before: one last round, all p-bits
        prev = round_err;
        // least squares for the endpoints given the current indices
        const uint8_t* wt = weights(ib);
        float a = 0, b = 0, cc = 0, x0[4] = {}, x1[4] = {};
        for (int i = 0; i < n; i++) {
            const float t = wt[cur[i]] / 64.0f, s = 1 - t;
            a += s * s, b += s * t, cc += t * t;
            for (int c = c0; c < c0 + nc; c++) x0[c] += s * x[i][c], x1[c] += t * x[i][c];
        }
        const float det = a * cc - b * b;
        if (std::fabs(det) < 1e-6f) break;
        for (int c = c0; c < c0 + nc; c++) {
            target[0][c] = std::clamp((cc * x0[c] - b * x1[c]) / det, 0.0f, 255.0f);
            target[1][c] = std::clamp((a * x1[c] - b * x0[c]) / det, 0.0f, 255.0f);
        }
    }
    return best;
}

struct Candidate {
    float err = 1e30f;
    uint8_t bits[16] = {};
};

// Packs a mode 0, 1, 2, 3, 6 or 7 encoding: `ends` per subset, `idx` per pixel (subset order resolved here).
void pack_joint(int mode, int part, int subsets, const Prec& pr, Ends* ends, uint8_t* idx, uint8_t out[16]) {
    const int ib = pr.ib, top = (1 << ib) - 1;
    // anchor pixels: index below the half, else swap the subset's endpoints and mirror its indices
    for (int s = 0; s < subsets; s++) {
        const int a = anchor_of(subsets, part, s);
        if (idx[a] >> (ib - 1)) {
            std::swap(ends[s].q[0], ends[s].q[1]), std::swap(ends[s].p[0], ends[s].p[1]);
            std::swap(ends[s].e[0], ends[s].e[1]);
            for (int i = 0; i < 16; i++)
                if (subset_of(subsets, part, i) == s) idx[i] = uint8_t(top - idx[i]);
        }
    }
    Writer w;
    w.put(1u << mode, mode + 1);
    if (subsets > 1) w.put(uint32_t(part), mode == 0 ? 4 : 6);
    for (int c = 0; c < 3; c++)
        for (int s = 0; s < subsets; s++)
            for (int k = 0; k < 2; k++) w.put(ends[s].q[k][c], pr.cb);
    if (pr.ab)
        for (int s = 0; s < subsets; s++)
            for (int k = 0; k < 2; k++) w.put(ends[s].q[k][3], pr.ab);
    if (pr.pkind == 1)
        for (int s = 0; s < subsets; s++)
            for (int k = 0; k < 2; k++) w.put(ends[s].p[k], 1);
    if (pr.pkind == 2)
        for (int s = 0; s < subsets; s++) w.put(ends[s].p[0], 1);
    for (int i = 0; i < 16; i++) {
        bool anchor = false;
        for (int s = 0; s < subsets; s++) anchor |= anchor_of(subsets, part, s) == i;
        w.put(idx[i], ib - (anchor ? 1 : 0));
    }
    std::memcpy(out, w.b, 16);
}

// A joint-index mode (0, 1, 2, 3, 6, 7) for one partition: fit every subset, pack.
void try_joint(const float (*px)[4], int mode, int part, const float* w, Candidate& best) {
    static const Prec kPrec[8] = {{4, 0, 1, 3, 3}, {6, 0, 2, 3, 3}, {5, 0, 0, 2, 3}, {7, 0, 1, 2, 3},
                                  {0, 0, 0, 0, 0}, {0, 0, 0, 0, 0}, {7, 7, 1, 4, 4}, {5, 5, 1, 2, 4}};
    const Prec& pr = kPrec[mode];
    const int subsets = mode == 0 || mode == 2 ? 3 : mode == 1 || mode == 3 || mode == 7 ? 2 : 1;
    Ends ends[3];
    uint8_t idx[16] = {};
    float err = 0;
    for (int s = 0; s < subsets; s++) {
        float sx[16][4];
        int map[16], n = 0;
        for (int i = 0; i < 16; i++)
            if (subset_of(subsets, part, i) == s) std::memcpy(sx[n], px[i], sizeof sx[n]), map[n++] = i;
        uint8_t sidx[16];
        err += fit(sx, n, pr, pr.ib, 0, pr.nc, w, ends[s], sidx, subsets > 1 ? 2 : 3);
        if (pr.nc == 3)  // opaque mode: alpha decodes to 255; its error counts
            for (int i = 0; i < n; i++) err += (255.0f - sx[i][3]) * (255.0f - sx[i][3]) * w[3];
        for (int i = 0; i < n; i++) idx[map[i]] = sidx[i];
        if (err >= best.err) return;
    }
    if (err >= best.err) return;
    best.err = err;
    pack_joint(mode, part, subsets, pr, ends, idx, best.bits);
}

// Modes 4 (sel 0 or 1) and 5, rotation 0: RGB and alpha with separate indices.
void try_separate(const float (*px)[4], int mode, int sel, const float* w, Candidate& best, int rotation = 0) {
    // mode 4: colour 5 bits, alpha 6 bits, index sets of 2 and 3 bits; mode 5: 7 and 8 bits, 2 and 2 bits
    const int cb = mode == 4 ? 5 : 7, ab = mode == 4 ? 6 : 8;
    const int ib1 = 2, ib2 = mode == 4 ? 3 : 2;
    const int cib = sel ? ib2 : ib1, aib = sel ? ib1 : ib2;  // colour and alpha index bits
    Prec cpr{cb, ab, 0, cib, 3}, apr{cb, ab, 0, aib, 1};
    Ends en;
    uint8_t cidx[16], aidx[16];
    float err = fit(px, 16, cpr, cib, 0, 3, w, en, cidx);
    if (err >= best.err) return;
    err += fit(px, 16, apr, aib, 3, 1, w, en, aidx);
    if (err >= best.err) return;
    best.err = err;
    // anchors (pixel 0) of each index set, separately: swap that group's endpoints
    if (cidx[0] >> (cib - 1)) {
        for (int c = 0; c < 3; c++) std::swap(en.q[0][c], en.q[1][c]);
        for (int i = 0; i < 16; i++) cidx[i] = uint8_t((1 << cib) - 1 - cidx[i]);
    }
    if (aidx[0] >> (aib - 1)) {
        std::swap(en.q[0][3], en.q[1][3]);
        for (int i = 0; i < 16; i++) aidx[i] = uint8_t((1 << aib) - 1 - aidx[i]);
    }
    Writer wr;
    wr.put(1u << mode, mode + 1);
    wr.put(uint32_t(rotation), 2);  // the decoder swaps alpha back with channel rotation - 1
    if (mode == 4) wr.put(uint32_t(sel), 1);
    for (int c = 0; c < 3; c++)
        for (int k = 0; k < 2; k++) wr.put(en.q[k][c], cb);
    for (int k = 0; k < 2; k++) wr.put(en.q[k][3], ab);
    // the first stored index set has ib1 bits, the second ib2; sel 1 stores colour in the second
    const uint8_t* first = sel ? aidx : cidx;
    const uint8_t* second = sel ? cidx : aidx;
    for (int i = 0; i < 16; i++) wr.put(first[i], ib1 - (i == 0 ? 1 : 0));
    for (int i = 0; i < 16; i++) wr.put(second[i], ib2 - (i == 0 ? 1 : 0));
    std::memcpy(best.bits, wr.b, 16);
}

// How well each partition's subsets lie on lines: the sum of each subset's residual off its principal axis.
void rank_partitions(const float (*px)[4], int subsets, int count, int nc, int keep, int* out) {
    // 1. every partition by the summed variance of its subsets (running sums: cheap)
    float coarse[64];
    for (int part = 0; part < count; part++) {
        float n[3] = {}, sum[3][4] = {}, sq[3][4] = {};
        for (int i = 0; i < 16; i++) {
            const int s = subset_of(subsets, part, i);
            n[s]++;
            for (int c = 0; c < nc; c++) sum[s][c] += px[i][c], sq[s][c] += px[i][c] * px[i][c];
        }
        float total = 0;
        for (int s = 0; s < subsets; s++)
            if (n[s] > 0)
                for (int c = 0; c < nc; c++) total += sq[s][c] - sum[s][c] * sum[s][c] / n[s];
        coarse[part] = total;
    }
    int order[64];
    for (int k = 0; k < count; k++) order[k] = k;
    const int fine = std::min(count, std::max(keep, kRankFine));
    std::partial_sort(order, order + fine, order + count, [&](int a, int b) { return coarse[a] < coarse[b]; });
    // 2. the best of those by how far their subsets lie off a line (what the endpoints can follow)
    float score[64];
    for (int k = 0; k < fine; k++) {
        const int part = order[k];
        float total = 0;
        for (int s = 0; s < subsets; s++) {
            float sx[16][4];
            int n = 0;
            for (int i = 0; i < 16; i++)
                if (subset_of(subsets, part, i) == s) std::memcpy(sx[n++], px[i], sizeof sx[0]);
            float mean[4], axis[4];
            principal_axis(sx, n, 0, nc, mean, axis);
            for (int i = 0; i < n; i++) {
                float t = 0, d2 = 0;
                for (int c = 0; c < nc; c++) t += (sx[i][c] - mean[c]) * axis[c];
                for (int c = 0; c < nc; c++) {
                    const float r = sx[i][c] - mean[c] - t * axis[c];
                    d2 += r * r;
                }
                total += d2;
            }
        }
        score[part] = total;
    }
    std::partial_sort(order, order + keep, order + fine, [&](int a, int b) { return score[a] < score[b]; });
    std::memcpy(out, order, sizeof(int) * size_t(keep));
}

}  // namespace

void bc7_encode_block(const uint8_t in[16][4], uint8_t out[16], Bc7EncodeStats* stats) {
    float px[16][4];
    bool opaque = true;
    for (int i = 0; i < 16; i++) {
        for (int c = 0; c < 4; c++) px[i][c] = float(in[i][c]);
        opaque &= in[i][3] == 255;
    }
    const float w[4] = {1, 1, 1, opaque ? kOpaqueAlphaWeight : 1.0f};
    Candidate best;
    try_joint(px, 6, 0, w, best);
    if (!opaque && best.err > kGoodEnough / 4) {  // mode 6 already near exact: the separate-alpha modes cannot matter
        try_separate(px, 5, 0, w, best);
        try_separate(px, 4, 0, w, best);
        try_separate(px, 4, 1, w, best);
    }
    int used = 1;
    bool tried2 = false, tried3 = false;
    if (best.err > kGoodEnough) {
        tried2 = true;
        int parts[kTry2];
        rank_partitions(px, 2, 64, opaque ? 3 : 4, kTry2, parts);
        const float before = best.err;
        for (int k = 0; k < kTry2; k++) {
            if (opaque) {
                try_joint(px, 1, parts[k], w, best);
                try_joint(px, 3, parts[k], w, best);
            } else {
                try_joint(px, 7, parts[k], w, best);
            }
        }
        if (best.err < before) used = 2;
    }
    if (best.err > kGoodEnough && opaque) {
        tried3 = true;
        int parts[kTry3];
        rank_partitions(px, 3, 64, 3, kTry3, parts);
        const float before = best.err;
        for (int k = 0; k < kTry3; k++) {
            try_joint(px, 2, parts[k], w, best);
            if (parts[k] < 16) try_joint(px, 0, parts[k], w, best);
        }
        if (best.err < before) used = 3;
    }
    bool rescued = false;
    if (best.err > kRescue) {  // every partition of every mode that applies, and the rotations of modes 4 and 5
        const float before = best.err;
        for (int part = 0; part < 64; part++) {
            if (opaque) {
                try_joint(px, 1, part, w, best);
                try_joint(px, 3, part, w, best);
                try_joint(px, 2, part, w, best);
                if (part < 16) try_joint(px, 0, part, w, best);
            } else {
                try_joint(px, 7, part, w, best);
            }
        }
        if (!opaque)
            for (int rot = 1; rot < 4; rot++) {  // the separately indexed channel is R, G or B instead of alpha
                float rx[16][4];
                for (int i = 0; i < 16; i++) {
                    for (int c = 0; c < 4; c++) rx[i][c] = px[i][c];
                    std::swap(rx[i][3], rx[i][rot - 1]);
                }
                try_separate(rx, 5, 0, w, best, rot);
                try_separate(rx, 4, 0, w, best, rot);
                try_separate(rx, 4, 1, w, best, rot);
            }
        rescued = best.err < before;
    }
    std::memcpy(out, best.bits, 16);
    if (stats) {
        stats->blocks++;
        (used == 1 ? stats->single : used == 2 ? stats->two : stats->three)++;
        stats->tried2 += tried2, stats->tried3 += tried3, stats->rescued += rescued;
    }
}

void bc7_encode_image(const uint8_t* rgba, uint32_t width, uint32_t height, uint8_t* blocks, Bc7EncodeStats* stats) {
    const uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            uint8_t px[16][4];
            for (uint32_t y = 0; y < 4; y++)
                for (uint32_t x = 0; x < 4; x++) {
                    const uint32_t sx = std::min(bx * 4 + x, width - 1), sy = std::min(by * 4 + y, height - 1);
                    std::memcpy(px[y * 4 + x], rgba + (uint64_t(sy) * width + sx) * 4, 4);
                }
            bc7_encode_block(px, blocks + (uint64_t(by) * bw + bx) * 16, stats);
        }
}

}  // namespace nx::nvn
