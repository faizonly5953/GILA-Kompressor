// =====================================================================
//  GILA - kompresor "context mixing" rasio ekstrem
//  Satu source untuk dua program:
//     -DGILA_MODE_COMPRESS   -> gila-kompres
//     -DGILA_MODE_DECOMPRESS -> gila-dekompres
//  Prinsip: tiap bit diprediksi oleh banyak model konteks (order 1..16,
//  kata, kolom/tabel, sparse, match panjang), prediksinya dicampur oleh
//  3 neural mixer + 1 mixer akhir, disaring 4 tahap APM/SSE, lalu
//  dikodekan dengan arithmetic coder 32-bit presisi 16-bit.
//  Semua aritmetika model berbasis integer -> hasil identik di semua PC.
// =====================================================================
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <new>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;
typedef uint8_t U8; typedef uint16_t U16; typedef uint32_t U32; typedef uint64_t U64; typedef int64_t I64;

static const int GILA_VERSION = 1;
#ifdef TUNE
static int envi(const char* n,int d){const char* v=getenv(n);return v?atoi(v):d;}
static int g_lr=envi("LR",4), g_lrf=envi("LRF",3), g_lim=envi("LIM",255), g_ar=envi("AR",6), g_sh=envi("SH",14), g_w0=envi("W0",12), g_fw=envi("FW",4);
#else
static const int g_lr=4, g_lrf=3, g_lim=255, g_ar=6, g_sh=14, g_w0=12, g_fw=4;
#endif

// ---------------------------------------------------------------- math
static int squash(int d) {               // 4096/(1+e^(-d/256))
    static const int t[33] = {1,2,3,6,10,16,27,45,73,120,194,310,488,747,1101,1546,
        2047,2549,2994,3348,3607,3785,3901,3975,4022,4050,4068,4079,4085,4089,4092,4093,4094};
    if (d > 2047) return 4095;
    if (d < -2047) return 1;
    int w = d & 127; d = (d >> 7) + 16;
    return (t[d] * (128 - w) + t[d + 1] * w + 64) >> 7;
}
static short stretchT[4096];
static int recT[1024];
static void initTables() {
    int pi = 0;
    for (int x = -2047; x <= 2047; ++x) {
        int v = squash(x);
        for (int i = pi; i <= v; ++i) stretchT[i] = (short)x;
        pi = v + 1;
    }
    for (int i = pi; i < 4096; ++i) stretchT[i] = 2047;
    for (int n = 0; n < 1024; ++n) recT[n] = (int)(131072 / (2 * n + 3)); // 65536/(n+1.5)
}
static inline int stretch(int p) { return stretchT[p]; }

static inline U64 hash64(U64 a, U64 b) {
    U64 h = (a + 0x9E3779B97F4A7C15ULL) * 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 31;
    h += b * 0x94D049BB133111EBULL;
    h ^= h >> 29;
    h *= 0xD6E8FEB86659FD93ULL;
    h ^= h >> 32;
    return h;
}

// Slot: 22-bit probabilitas + 10-bit hitungan (laju belajar adaptif 1/(n+1.5))
static const U32 SLOT_INIT = 1u << 31;
static inline void slotUpdate(U32* s, int y, U32 limit) {
    U32 v = *s;
    U32 n = v & 1023;
    int p = (int)(v >> 10);
    int target = y ? (1 << 22) - 1 : 0;
    p += (int)(((I64)(target - p) * recT[n]) >> 16);
    if (n < limit) ++n;
    *s = ((U32)p << 10) | n;
}
static inline int slotP(U32 s) { int p = (int)(s >> 20); return p < 1 ? 1 : (p > 4095 ? 4095 : p); }

// Slot konteks: p16 langsung (16 bit) | hitungan n (8 bit) | riwayat 8 bit terakhir
static inline void cslotUpdate(U32* s, int y, U32 limit) {
    U32 v = *s;
    U32 n = (v >> 8) & 255;
    int p = (int)(v >> 16);
    p += (int)(((I64)((y ? 65535 : 0) - p) * recT[n]) >> 16);
    if (n < limit) ++n;
    *s = ((U32)p << 16) | (n << 8) | (((v << 1) | (U32)y) & 255);
}
static inline int cslotP(U32 s) { int p = (int)(s >> 20); return p < 1 ? 1 : (p > 4095 ? 4095 : p); }
static inline int histIdx(U32 s) {        // riwayat bit -> indeks StateMap (0..1023)
    U32 n = (s >> 8) & 255, h = s & 255;
    if (n < 8) return (int)((1u << n) | (h & ((1u << n) - 1)));
    int nq = n < 16 ? 0 : (n < 48 ? 1 : 2);
    return 256 + nq * 256 + (int)h;
}

// ---------------------------------------------------------------- mixer
struct Mixer {
    int n = 0, nsets = 0, sel = 0, pr = 2048, dotv = 0, lr = 6;
    std::vector<int> w;
    const int* x = nullptr;
    void init(int n_, int sets, const int* x_, int w0, int lr_) {
        n = n_; nsets = sets; x = x_; lr = lr_;
        w.assign((size_t)n * sets, w0);
    }
    int mix() {
        const int* ww = &w[(size_t)sel * n];
        I64 s = 0;
        for (int i = 0; i < n; ++i) s += (I64)x[i] * ww[i];
        s >>= 16;
        if (s > 2047) s = 2047;
        if (s < -2047) s = -2047;
        dotv = (int)s;
        pr = squash(dotv);
        return dotv;
    }
    void update(int y) {
        int err = ((y << 12) - pr) * lr;
        int* ww = &w[(size_t)sel * n];
        for (int i = 0; i < n; ++i) ww[i] += (int)(((I64)x[i] * err + (1 << (g_sh - 1))) >> g_sh);
    }
};

// ---------------------------------------------------------------- APM / SSE (16-bit)
struct APM {
    std::vector<U16> t; int idx = 0, rate = 7;
    void init(int n, int rate_) {
        rate = rate_;
        t.resize((size_t)n * 33);
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < 33; ++j) {
                int v = squash((j - 16) * 128) * 16;
                t[(size_t)i * 33 + j] = (U16)(v > 65535 ? 65535 : v);
            }
    }
    int pp(int st, int cx) {          // st: domain stretch [-2047,2047]
        int s = st + 2048;            // 1..4095
        int lo = s >> 7, w = s & 127;
        size_t b = (size_t)cx * 33 + lo;
        idx = (int)(b + (w >> 6));
        return (int)((t[b] * (128 - w) + t[b + 1] * w) >> 7);
    }
    void update(int y) {
        int g = y ? 65535 : 0;
        int v = t[idx];
        v += (g - v) >> rate;
        t[idx] = (U16)v;
    }
};

// ---------------------------------------------------------------- predictor
struct Bucket { U16 chk; U16 pad; U32 s[15]; };   // 64 byte = 1 cache line

static const int NH = 18;                // jumlah konteks ber-hash
static const int NIN = (NH + 2) * 3 + 3; // input mixer

struct Predictor {
    int L = 8;
    Bucket* T = nullptr; U64 tmask = 0;
    U8* buf = nullptr; U64 bufmask = 0;
    U32* mht = nullptr; U32 mhmask = 0;

    int c0 = 1, bitpos = 0, nib = 1;
    U32 c4 = 0, c8 = 0, c12 = 0, c16 = 0;
    U64 pos = 0;
    U32 wordh = 0, word2 = 0, word3 = 0;
    U64 lineStart = 0, prevLineLen = 0;
    std::vector<U16> ih1, ih2;            // byte yang dulu mengikuti konteks order-1 / order-2
    std::vector<U64> lastPos; std::vector<U32> lastDist;
    U32 rlen = 0, rcand = 0; int rhits = 0;  // record model

    U64 ctxv[NH], ctxh[NH];
    Bucket* cb[NH];
    U32* sl[NH + 2];
    int found = 0;
    std::vector<U32> t0, t1, sm;
    int smi[NH + 2];

    U64 mptr = 0; U32 mlen = 0; int eb = 0; int mctx = -1, mexp = 0, mlb = 0;
    U32 msm[64];

    int x[NIN];
    int xf[4];
    Mixer m1, m2, m3, mf;
    APM a1, a2, a3, a4;
    int pr16 = 32768, prMix12 = 2048;

    bool init(int level) {
        L = level;
        U64 nb = 1ULL << (15 + L);
        int lb = L > 9 ? 9 : L;
        T = (Bucket*)malloc(nb * sizeof(Bucket));
        U64 bsz = 1ULL << (19 + lb);
        buf = (U8*)calloc(bsz, 1);
        U32 mhn = 1u << (14 + lb);
        mht = (U32*)calloc(mhn, sizeof(U32));
        if (!T || !buf || !mht) return false;
        tmask = nb - 1; bufmask = bsz - 1; mhmask = mhn - 1;
        for (U64 i = 0; i < nb; ++i) {
            T[i].chk = 0; T[i].pad = 0;
            for (int j = 0; j < 15; ++j) T[i].s[j] = SLOT_INIT;
        }
        ih1.assign(256, 0); ih2.assign(65536, 0);
        lastPos.assign(256, 0); lastDist.assign(256, 0);
        t0.assign(256, SLOT_INIT);
        t1.assign(65536, SLOT_INIT);
        sm.assign((size_t)(NH + 2) * 1024, SLOT_INIT);
        for (int i = 0; i < 64; ++i) msm[i] = SLOT_INIT;
        m1.init(NIN, (NH + 1) * 4, x, 1 << g_w0, g_lr);
        m2.init(NIN, 256, x, 1 << g_w0, g_lr);
        m3.init(NIN, 256 * 8, x, 1 << g_w0, g_lr);
        mf.init(4, 4 * 8, xf, 1 << 15, g_lrf);
        a1.init(256, g_ar);
        a2.init(65536, g_ar);
        a3.init(64 * 256, g_ar);
        a4.init(1 << 20, g_ar);
        computeContexts();
        lookup();
        predict();
        return true;
    }
    ~Predictor() { free(T); free(buf); free(mht); }

    Bucket* find(U64 k, bool& ok) {
        U16 chk = (U16)(k >> 48);
        U64 i = k & tmask;
        Bucket* b1 = &T[i];
        if (b1->chk == chk) { ok = true; return b1; }
        Bucket* b2 = &T[i ^ 1];
        if (b2->chk == chk) { ok = true; return b2; }
        Bucket* r = (((b1->s[0] >> 8) & 255) <= ((b2->s[0] >> 8) & 255)) ? b1 : b2;
        r->chk = chk;
        for (int j = 0; j < 15; ++j) r->s[j] = SLOT_INIT;
        ok = false;
        return r;
    }

    static inline bool isWordChar(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 128 || (c >= '0' && c <= '9'); }

    void computeContexts() {
        U32 c1 = c4 & 0xff;
        U64 col = pos - lineStart;
        U8 above = 0, above2 = 0;
        if (prevLineLen > 0 && prevLineLen <= pos) {
            above = buf[(pos - prevLineLen) & bufmask];
            above2 = buf[(pos - prevLineLen + 1) & bufmask];
        }
        U64 colq = col > 255 ? 255 : col;
        ctxv[0]  = c4 & 0xffff;                                   // order 2
        ctxv[1]  = c4 & 0xffffff;                                 // order 3
        ctxv[2]  = c4;                                            // order 4
        ctxv[3]  = c4 | ((U64)(c8 & 0xff) << 32);                 // order 5
        ctxv[4]  = c4 | ((U64)(c8 & 0xffff) << 32);               // order 6
        ctxv[5]  = c4 | ((U64)c8 << 32);                          // order 8
        ctxv[6]  = hash64(c4 | ((U64)c8 << 32), c12);             // order 12
        ctxv[7]  = hash64(c4 | ((U64)c8 << 32), c12 | ((U64)c16 << 32)); // order 16
        ctxv[8]  = wordh ? (U64)wordh : (0x100000000ULL | c1);    // kata
        ctxv[9]  = hash64(wordh, word2);                          // bigram kata
        ctxv[10] = hash64(word2 | ((U64)word3 << 32), wordh ? 1 : (c1 + 2)); // skip-word
        ctxv[11] = c4 & 0xffff00;                                 // sparse (byte 2,3)
        ctxv[12] = (c4 & 0xff00ff) | ((U64)(c8 & 0xff) << 32);    // sparse
        ctxv[13] = above | ((U64)above2 << 8) | (colq << 16) | ((U64)c1 << 24); // kolom/tabel
        ctxv[14] = c1 | ((U64)ih1[c1] << 8);                                     // indirect order-1
        ctxv[15] = (c4 & 0xffff) | ((U64)ih2[c4 & 0xffff] << 16);                // indirect order-2
        U32 r1 = 0, r2 = 0;
        if (rlen && pos >= 2ULL * rlen) { r1 = buf[(pos - rlen) & bufmask]; r2 = buf[(pos - 2 * rlen) & bufmask]; }
        ctxv[16] = rlen ? hash64(rlen, r1 | (r2 << 8)) : 0;                       // record: kolom vertikal
        ctxv[17] = rlen ? hash64((U64)rlen << 1 | 1, r1 | (c1 << 8)) : 1;          // record + byte terakhir
        for (int i = 0; i < NH; ++i) ctxh[i] = hash64(ctxv[i], (U64)(i + 1) * 0x10001ULL);
    }

    void lookup() {
        found = 0;
        for (int i = 0; i < NH; ++i) {
            bool ok;
            U64 k = bitpos == 0 ? ctxh[i] : hash64(ctxh[i], (U64)c0 + 0x7777);
            cb[i] = find(k, ok);
            if (ok) ++found;
        }
        nib = 1;
    }

    void byteUpdate() {
        int c = c0 & 255;
        buf[pos & bufmask] = (U8)c;
        {   // indirect & record model
            U32 o1 = c4 & 0xff, o2 = c4 & 0xffff;
            ih1[o1] = (U16)((ih1[o1] << 8) | c);
            ih2[o2] = (U16)((ih2[o2] << 8) | c);
            if (lastPos[c]) {
                U64 d = pos + 1 - lastPos[c];
                if (d > 1 && d < 65536 && d == lastDist[c]) {
                    if (d == rcand) { if (++rhits >= 4) rlen = rcand; }
                    else { rcand = (U32)d; rhits = 1; }
                }
                lastDist[c] = (U32)d;
            }
            lastPos[c] = pos + 1;
        }
        ++pos;
        c16 = (c16 << 8) | (c12 >> 24);
        c12 = (c12 << 8) | (c8 >> 24);
        c8 = (c8 << 8) | (c4 >> 24);
        c4 = (c4 << 8) | (U32)c;
        if (isWordChar(c)) {
            int lc = (c >= 'A' && c <= 'Z') ? c + 32 : c;
            wordh = (wordh + (U32)lc + 1) * 0x2F0F3A27u;
            if (!wordh) wordh = 1;
        } else if (wordh) {
            word3 = word2; word2 = wordh; wordh = 0;
        }
        if (c == '\n') { prevLineLen = pos - lineStart; lineStart = pos; }

        // match model
        if (mlen > 0) {
            if (buf[mptr & bufmask] == (U8)c) { if (mlen < 65535) ++mlen; ++mptr; }
            else mlen = 0;
        }
        if (pos >= 6) {
            U32 h = (U32)(hash64(c4, c8 & 0xffff) >> 20) & mhmask;
            if (mlen == 0) {
                U32 st = mht[h];
                U32 dist = (U32)pos - st;
                if (st != 0 && dist > 0 && (U64)dist < bufmask - 65536) {
                    U64 p = pos - dist;
                    U32 l = 0;
                    while (l < 65535 && l < p && buf[(p - 1 - l) & bufmask] == buf[(pos - 1 - l) & bufmask]) ++l;
                    if (l >= 6) { mlen = l; mptr = p; }
                }
            }
            mht[h] = (U32)pos;
        }
        if (mlen > 0) eb = buf[mptr & bufmask];
        c0 = 1; bitpos = 0;
        computeContexts();
    }

    void predict() {
        for (int i = 0; i < NH; ++i) sl[i] = &cb[i]->s[nib - 1];
        sl[NH] = &t0[c0];
        sl[NH + 1] = &t1[((c4 & 0xff) << 8) | c0];

        mctx = -1; mlb = 0;
        if (mlen > 0 && ((eb + 256) >> (8 - bitpos)) == c0) {
            mexp = (eb >> (7 - bitpos)) & 1;
            int lq = mlen < 16 ? (int)mlen : 16 + (int)((mlen - 16) >> 3);
            if (lq > 31) lq = 31;
            mctx = lq * 2 + mexp;
            mlb = mlen < 16 ? 1 : (mlen < 32 ? 2 : 3);
        }

        int k = 0;
        for (int i = 0; i < NH + 2; ++i) {
            U32 sv = *sl[i];
            int p = cslotP(sv);
            x[k++] = stretch(p);
            x[k++] = (p - 2048) >> 1;
            smi[i] = i * 1024 + histIdx(sv);
            x[k++] = stretch(slotP(sm[smi[i]]));
        }
        if (mctx >= 0) {
            int p = slotP(msm[mctx]);
            x[k++] = stretch(p);
            int ml = mlen > 32 ? 32 : (int)mlen;
            x[k++] = (mexp ? 1 : -1) * ml * 32;
        } else { x[k++] = 0; x[k++] = 0; }
        x[k++] = 256;

        U32 c1 = c4 & 0xff;
        m1.sel = found * 4 + mlb;
        m2.sel = c0;
        m3.sel = (int)(c1 * 8 + bitpos);
        xf[0] = m1.mix(); xf[1] = m2.mix(); xf[2] = m3.mix(); xf[3] = 256;
        mf.sel = mlb * 8 + bitpos;
        int st = mf.mix();
        prMix12 = mf.pr;

        int p1 = a1.pp(st, c0);
        int p2 = a2.pp(st, (int)(c0 | (c1 << 8)));
        int p3 = a3.pp(st, (mctx < 0 ? 0 : mctx) * 256 + c0);
        int p4 = a4.pp(st, (int)((hash64(c4 & 0xffff, 99) & 0xfff) << 8) | c0);
        int pm = prMix12 * 16;
        int p;
        switch (g_fw) {
            case 1: p = (pm + p1 + p2 + p3 + p4 * 4) >> 3; break;
            case 2: p = (pm * 4 + p2 + p3 + p4 * 2) >> 3; break;
            case 3: p = (pm * 2 + p2 * 2 + p3 * 2 + p4 * 2) >> 3; break;
            case 4: p = (p1 + p2 * 2 + p3 * 2 + p4 * 3) >> 3; break;
            default: p = (pm * 2 + p1 + p2 * 2 + p3 + p4 * 2) >> 3;
        }
        if (p < 1) p = 1;
        if (p > 65535) p = 65535;
        pr16 = p;
    }

    void update(int y) {
        for (int i = 0; i < NH; ++i) cslotUpdate(sl[i], y, (U32)g_lim);
        cslotUpdate(sl[NH], y, 255);
        cslotUpdate(sl[NH + 1], y, 255);
        for (int i = 0; i < NH + 2; ++i) slotUpdate(&sm[smi[i]], y, 1023);
        if (mctx >= 0) slotUpdate(&msm[mctx], y, 1023);
        m1.update(y); m2.update(y); m3.update(y); mf.update(y);
        a1.update(y); a2.update(y); a3.update(y); a4.update(y);

        c0 = (c0 << 1) | y; ++bitpos; nib = (nib << 1) | y;
        if (bitpos == 8) { byteUpdate(); lookup(); }
        else if (bitpos == 4) lookup();
        predict();
    }
    int p() const { return pr16; }
};

// ---------------------------------------------------------------- arithmetic coder
struct Encoder {
    FILE* f; U32 x1 = 0, x2 = 0xffffffff; U64 outBytes = 0;
    explicit Encoder(FILE* f_) : f(f_) {}
    void bit(int y, int p16) {
        U32 xmid = x1 + (U32)(((U64)(x2 - x1) * (U32)p16) >> 16);
        y ? (x2 = xmid) : (x1 = xmid + 1);
        while (((x1 ^ x2) & 0xff000000) == 0) {
            putc(x2 >> 24, f); ++outBytes;
            x1 <<= 8; x2 = (x2 << 8) | 255;
        }
    }
    void flush() { for (int i = 0; i < 4; ++i) { putc(x1 >> 24, f); x1 <<= 8; ++outBytes; } }
};
struct Decoder {
    FILE* f; U32 x1 = 0, x2 = 0xffffffff, x = 0;
    explicit Decoder(FILE* f_) : f(f_) { for (int i = 0; i < 4; ++i) x = (x << 8) | (U32)(getc(f) & 255); }
    int bit(int p16) {
        U32 xmid = x1 + (U32)(((U64)(x2 - x1) * (U32)p16) >> 16);
        int y = x <= xmid;
        y ? (x2 = xmid) : (x1 = xmid + 1);
        while (((x1 ^ x2) & 0xff000000) == 0) {
            x1 <<= 8; x2 = (x2 << 8) | 255;
            int c = getc(f); x = (x << 8) | (U32)(c == EOF ? 0 : c);
        }
        return y;
    }
};

// ---------------------------------------------------------------- crc32
static U32 crcT[256];
static void initCrc() { for (U32 i = 0; i < 256; ++i) { U32 c = i; for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crcT[i] = c; } }
static inline U32 crcUpd(U32 crc, U8 b) { return crcT[(crc ^ b) & 0xff] ^ (crc >> 8); }

// ---------------------------------------------------------------- util
static FILE* openFile(const fs::path& p, const char* mode) {
#ifdef _WIN32
    wchar_t wm[8]; int i = 0; for (; mode[i] && i < 7; ++i) wm[i] = (wchar_t)mode[i]; wm[i] = 0;
    return _wfopen(p.c_str(), wm);
#else
    return fopen(p.c_str(), mode);
#endif
}
static std::string toU8(const fs::path& p) {
    std::string s = p.u8string();
    for (auto& ch : s) if (ch == '\\') ch = '/';
    return s;
}
static fs::path fromU8(const std::string& s) { return fs::u8path(s); }

static std::string human(U64 b) {
    char t[64];
    if (b < 1024) snprintf(t, sizeof t, "%llu B", (unsigned long long)b);
    else if (b < 1048576) snprintf(t, sizeof t, "%.2f KB", b / 1024.0);
    else if (b < 1073741824ULL) snprintf(t, sizeof t, "%.2f MB", b / 1048576.0);
    else snprintf(t, sizeof t, "%.2f GB", b / 1073741824.0);
    return t;
}

struct Progress {
    std::chrono::steady_clock::time_point t0, last;
    U64 total;
    explicit Progress(U64 tot) : total(tot) { t0 = last = std::chrono::steady_clock::now(); }
    void show(U64 done, U64 outb, bool force = false) {
        auto now = std::chrono::steady_clock::now();
        if (!force && std::chrono::duration<double>(now - last).count() < 0.5) return;
        last = now;
        double el = std::chrono::duration<double>(now - t0).count();
        double pct = total ? 100.0 * done / total : 100.0;
        double spd = el > 0 ? done / el / 1048576.0 : 0;
        double eta = (spd > 0 && total > done) ? (total - done) / 1048576.0 / spd : 0;
        printf("\r  %5.1f%%  %s -> %s   %.2f MB/s   sisa ~%.0f dtk     ", pct, human(done).c_str(), human(outb).c_str(), spd, eta);
        fflush(stdout);
    }
    double elapsed() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }
};

static void pauseIfOwnConsole() {
#ifdef _WIN32
    DWORD list[4];
    if (GetConsoleProcessList(list, 4) <= 1) {
        printf("\nTekan Enter untuk menutup...");
        fflush(stdout);
        getchar();
    }
#endif
}

static fs::path uniquePath(fs::path p) {
    if (!fs::exists(p)) return p;
    fs::path dir = p.parent_path();
    std::string stem = p.stem().u8string(), ext = p.extension().u8string();
    for (int i = 2; i < 10000; ++i) {
        fs::path q = dir / fromU8(stem + " (" + std::to_string(i) + ")" + ext);
        if (!fs::exists(q)) return q;
    }
    return p;
}

static const char MAGIC[4] = {'G', 'I', 'L', 'A'};

// type: 0 = file biasa, 1 = folder, 2 = file executable (difilter E8/E9)
struct Entry { int type; std::string path; U64 size; fs::path src; };

// Filter E8/E9: alamat relatif CALL/JMP x86 -> absolut, supaya target yang sama
// menghasilkan byte yang sama (lebih mudah diprediksi). Reversibel sempurna.
static void e8e9(std::vector<U8>& b, bool enc) {
    size_t n = b.size();
    for (size_t i = 0; i + 4 < n;) {
        if ((b[i] & 0xFE) == 0xE8) {
            U32 a = (U32)b[i + 1] | ((U32)b[i + 2] << 8) | ((U32)b[i + 3] << 16) | ((U32)b[i + 4] << 24);
            a = enc ? a + (U32)i : a - (U32)i;
            b[i + 1] = (U8)a; b[i + 2] = (U8)(a >> 8); b[i + 3] = (U8)(a >> 16); b[i + 4] = (U8)(a >> 24);
            i += 5;
        } else ++i;
    }
}
static bool looksExecutable(const fs::path& p, U64 size) {
    if (size < 512 || size > (512ULL << 20)) return false;
    FILE* f = openFile(p, "rb");
    if (!f) return false;
    U8 h[4] = {0, 0, 0, 0};
    size_t got = fread(h, 1, 4, f);
    fclose(f);
    if (got < 4) return false;
    return (h[0] == 'M' && h[1] == 'Z') || (h[0] == 0x7F && h[1] == 'E' && h[2] == 'L' && h[3] == 'F');
}

// ---------------------------------------------------------------- compress
static int doCompress(const std::vector<fs::path>& inputs, fs::path out, int level) {
    std::vector<Entry> ents;
    for (auto& in0 : inputs) {
        std::error_code ec;
        fs::path in = fs::absolute(in0, ec);
        if (!fs::exists(in)) { printf("  [!] Tidak ditemukan: %s\n", toU8(in0).c_str()); return 1; }
        std::string base = toU8(in.filename());
        if (base.empty()) base = toU8(in.parent_path().filename());
        if (fs::is_directory(in)) {
            ents.push_back({1, base, 0, in});
            std::vector<Entry> sub;
            // penelusuran manual (lebih andal daripada recursive_directory_iterator di Windows)
            std::vector<std::pair<fs::path, std::string>> stack{{in, base}};
            while (!stack.empty()) {
                auto [dir, relDir] = stack.back();
                stack.pop_back();
                std::error_code e2;
                fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, e2), endIt;
                if (e2) { printf("  [!] Tidak bisa membaca folder %s\n", relDir.c_str()); continue; }
                for (; it != endIt; it.increment(e2)) {
                    if (e2) break;
                    fs::path p = it->path();
                    std::string rel = relDir + "/" + toU8(p.filename());
                    std::error_code e3;
                    fs::file_status stt = fs::status(p, e3);
                    if (e3) continue;
                    if (fs::is_directory(stt)) { sub.push_back({1, rel, 0, p}); stack.push_back({p, rel}); }
                    else if (fs::is_regular_file(stt)) { U64 sz = (U64)fs::file_size(p, e3); if (!e3) sub.push_back({0, rel, sz, p}); }
                }
            }
            // folder dulu, lalu file dikelompokkan per ekstensi (rasio lebih baik)
            std::stable_sort(sub.begin(), sub.end(), [](const Entry& a, const Entry& b) {
                if (a.type != b.type) return a.type > b.type;
                if (a.type == 0) {
                    auto extOf = [](const std::string& s) {
                        size_t sl = s.rfind('/'), dt = s.rfind('.');
                        return (dt == std::string::npos || (sl != std::string::npos && dt < sl)) ? std::string() : s.substr(dt);
                    };
                    std::string ea = extOf(a.path), eb = extOf(b.path);
                    if (ea != eb) return ea < eb;
                }
                return a.path < b.path;
            });
            for (auto& e : sub) ents.push_back(e);
        } else {
            ents.push_back({0, base, (U64)fs::file_size(in, ec), in});
        }
    }
    U64 total = 0; int nfiles = 0, nexe = 0;
    for (auto& e : ents) if (e.type == 0) {
        if (looksExecutable(e.src, e.size)) { e.type = 2; ++nexe; }
        total += e.size; ++nfiles;
    }

    // header (ikut dikompres)
    std::vector<U8> hdr;
    auto put32 = [&](U32 v) { for (int i = 0; i < 4; ++i) hdr.push_back((U8)(v >> (8 * i))); };
    auto put64 = [&](U64 v) { for (int i = 0; i < 8; ++i) hdr.push_back((U8)(v >> (8 * i))); };
    put32((U32)ents.size());
    for (auto& e : ents) {
        hdr.push_back((U8)e.type);
        put32((U32)e.path.size());
        for (char ch : e.path) hdr.push_back((U8)ch);
        if (e.type != 1) put64(e.size);
    }

    FILE* fo = openFile(out, "wb");
    if (!fo) { printf("  [!] Tidak bisa membuat file output: %s\n", toU8(out).c_str()); return 1; }
    static char obuf[1 << 20]; setvbuf(fo, obuf, _IOFBF, sizeof obuf);
    fwrite(MAGIC, 1, 4, fo);
    putc(GILA_VERSION, fo); putc(level, fo);

    int lb = level > 9 ? 9 : level;
    U64 mem = (1ULL << (15 + level)) * 64 + (1ULL << (19 + lb)) + (1ULL << (16 + lb)) + 45000000ULL;
    printf("  Input  : %d file, total %s\n", nfiles, human(total).c_str());
    printf("  Output : %s\n", toU8(out).c_str());
    printf("  Level  : %d (RAM ~%s)\n", level, human(mem).c_str());
    printf("  Menyiapkan model...\n");

    Predictor* P = new (std::nothrow) Predictor();
    if (!P || !P->init(level)) { printf("  [!] Memori tidak cukup. Coba level lebih kecil, mis. -m6\n"); fclose(fo); return 1; }
    Encoder E(fo);
    auto encByte = [&](int c) {
        for (int i = 7; i >= 0; --i) { int y = (c >> i) & 1; E.bit(y, P->p()); P->update(y); }
    };
    for (U8 b : hdr) encByte(b);

    Progress pg(total);
    U64 done = 0; U32 crc = 0xffffffff;
    std::vector<U8> rb(1 << 16);
    for (auto& e : ents) {
        if (e.type == 1) continue;
        FILE* fi = openFile(e.src, "rb");
        U64 left = e.size;
        if (!fi) printf("\n  [!] Gagal membuka %s (diisi nol)\n", e.path.c_str());
        if (e.type == 2) {       // executable: baca utuh, filter E8/E9, lalu kodekan
            std::vector<U8> fb((size_t)e.size, 0);
            size_t got = fi ? fread(fb.data(), 1, fb.size(), fi) : 0; (void)got;
            for (U8 b : fb) crc = crcUpd(crc, b);
            e8e9(fb, true);
            for (size_t i = 0; i < fb.size(); ++i) {
                encByte(fb[i]);
                if ((i & 0xffff) == 0) pg.show(done + i, E.outBytes + 6);
            }
            done += e.size; left = 0;
        }
        while (left > 0) {
            size_t want = left < rb.size() ? (size_t)left : rb.size();
            size_t got = fi ? fread(rb.data(), 1, want, fi) : 0;
            if (got < want) memset(rb.data() + got, 0, want - got);
            for (size_t i = 0; i < want; ++i) { encByte(rb[i]); crc = crcUpd(crc, rb[i]); }
            left -= want; done += want;
            pg.show(done, E.outBytes + 6);
        }
        if (fi) fclose(fi);
    }
    crc ^= 0xffffffff;
    for (int i = 0; i < 4; ++i) encByte((crc >> (8 * i)) & 0xff);
    E.flush();
    pg.show(done, E.outBytes + 6, true);
    fclose(fo);
    delete P;
    U64 outSize = E.outBytes + 6;
    printf("\n\n  SELESAI dalam %.1f detik\n", pg.elapsed());
    printf("  %s  ->  %s", human(total).c_str(), human(outSize).c_str());
    if (total) printf("   (%.2f%% dari asli, rasio %.2f : 1, %.3f bit/byte)", 100.0 * outSize / total, (double)total / outSize, 8.0 * outSize / total);
    printf("\n");
    return 0;
}

// ---------------------------------------------------------------- decompress
static bool safeRel(const std::string& s) {
    if (s.empty() || s[0] == '/' || s.find(':') != std::string::npos) return false;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find('/', i); if (j == std::string::npos) j = s.size();
        if (s.compare(i, j - i, "..") == 0 && j - i == 2) return false;
        i = j + 1;
    }
    return true;
}

static int doDecompress(const fs::path& in, fs::path outDir) {
    FILE* fi = openFile(in, "rb");
    if (!fi) { printf("  [!] Tidak bisa membuka %s\n", toU8(in).c_str()); return 1; }
    static char ibuf[1 << 20]; setvbuf(fi, ibuf, _IOFBF, sizeof ibuf);
    char mg[4];
    if (fread(mg, 1, 4, fi) != 4 || memcmp(mg, MAGIC, 4) != 0) { printf("  [!] Bukan file .gila yang valid\n"); fclose(fi); return 1; }
    int ver = getc(fi), level = getc(fi);
    if (ver != GILA_VERSION || level < 1 || level > 11) { printf("  [!] Versi/level arsip tidak didukung (v%d, level %d)\n", ver, level); fclose(fi); return 1; }
    std::error_code ec;
    U64 inSize = fs::file_size(in, ec);

    printf("  Arsip  : %s (%s, level %d)\n", toU8(in).c_str(), human(inSize).c_str(), level);
    printf("  Tujuan : %s\n", toU8(outDir).c_str());
    printf("  Menyiapkan model...\n");
    Predictor* P = new (std::nothrow) Predictor();
    if (!P || !P->init(level)) { printf("  [!] Memori tidak cukup untuk level %d\n", level); fclose(fi); return 1; }
    Decoder D(fi);
    auto decByte = [&]() { int c = 0; for (int i = 0; i < 8; ++i) { int y = D.bit(P->p()); P->update(y); c = (c << 1) | y; } return c; };
    auto get32 = [&]() { U32 v = 0; for (int i = 0; i < 4; ++i) v |= (U32)decByte() << (8 * i); return v; };
    auto get64 = [&]() { U64 v = 0; for (int i = 0; i < 8; ++i) v |= (U64)decByte() << (8 * i); return v; };

    U32 n = get32();
    if (n > 50000000) { printf("  [!] Arsip rusak\n"); return 1; }
    std::vector<Entry> ents(n);
    U64 total = 0;
    for (U32 i = 0; i < n; ++i) {
        ents[i].type = decByte();
        U32 len = get32();
        if (len > 65536) { printf("  [!] Arsip rusak\n"); return 1; }
        ents[i].path.resize(len);
        for (U32 k = 0; k < len; ++k) ents[i].path[k] = (char)decByte();
        if (ents[i].type > 2) { printf("  [!] Arsip rusak\n"); return 1; }
        ents[i].size = ents[i].type != 1 ? get64() : 0;
        total += ents[i].size;
    }
    fs::create_directories(outDir, ec);
    Progress pg(total);
    U64 done = 0; U32 crc = 0xffffffff;
    std::vector<U8> wb(1 << 16);
    int nfiles = 0;
    for (auto& e : ents) {
        bool ok = safeRel(e.path);
        fs::path dst = outDir / fromU8(e.path);
        if (e.type == 1) { if (ok) fs::create_directories(dst, ec); continue; }
        if (ok) fs::create_directories(dst.parent_path(), ec);
        FILE* fo = ok ? openFile(dst, "wb") : nullptr;
        if (!fo) printf("\n  [!] Tidak bisa menulis %s (dilewati)\n", e.path.c_str());
        U64 left = e.size;
        if (e.type == 2) {       // executable: dekode utuh, balikkan filter E8/E9
            std::vector<U8> fb((size_t)e.size);
            for (size_t i = 0; i < fb.size(); ++i) {
                fb[i] = (U8)decByte();
                if ((i & 0xffff) == 0) pg.show(done + i, done + i);
            }
            e8e9(fb, false);
            for (U8 b : fb) crc = crcUpd(crc, b);
            if (fo) fwrite(fb.data(), 1, fb.size(), fo);
            done += e.size; left = 0;
        }
        while (left > 0) {
            size_t k = left < wb.size() ? (size_t)left : wb.size();
            for (size_t i = 0; i < k; ++i) { wb[i] = (U8)decByte(); crc = crcUpd(crc, wb[i]); }
            if (fo) fwrite(wb.data(), 1, k, fo);
            left -= k; done += k;
            pg.show(done, done);
        }
        if (fo) fclose(fo);
        ++nfiles;
    }
    crc ^= 0xffffffff;
    U32 stored = get32();
    pg.show(done, done, true);
    fclose(fi);
    delete P;
    printf("\n\n  SELESAI dalam %.1f detik: %d file, %s\n", pg.elapsed(), nfiles, human(total).c_str());
    if (crc == stored) { printf("  Verifikasi CRC32: OK (data identik 100%%)\n"); return 0; }
    printf("  [!] CRC32 TIDAK cocok - arsip rusak!\n");
    return 2;
}

// ---------------------------------------------------------------- main
#if !defined(GILA_MODE_COMPRESS) && !defined(GILA_MODE_DECOMPRESS)
#define GILA_MODE_COMPRESS
#endif

static std::vector<fs::path> getArgs(int argc, char** argv) {
    std::vector<fs::path> a;
#ifdef _WIN32
    (void)argc; (void)argv;
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    for (int i = 1; i < n; ++i) a.emplace_back(std::wstring(w[i]));
    LocalFree(w);
#else
    for (int i = 1; i < argc; ++i) a.emplace_back(argv[i]);
#endif
    return a;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(65001);
#endif
    initTables(); initCrc();
    std::vector<fs::path> args = getArgs(argc, argv);
    int rc = 0;
#ifdef GILA_MODE_COMPRESS
    printf("GILA Kompres v%d - context mixing, rasio ekstrem\n\n", GILA_VERSION);
    int level = 8; fs::path out; std::vector<fs::path> ins;
    for (size_t i = 0; i < args.size(); ++i) {
        std::string s = toU8(args[i]);
        if (s.size() >= 3 && s[0] == '-' && s[1] == 'm') { level = atoi(s.c_str() + 2); if (level < 1) level = 1; if (level > 11) level = 11; }
        else if (s == "-o" && i + 1 < args.size()) out = args[++i];
        else ins.push_back(args[i]);
    }
    if (ins.empty()) {
        printf("Cara pakai:\n"
               "  - Seret (drag & drop) file/folder ke ikon gila-kompres.exe, ATAU\n"
               "  - gila-kompres [-m1..-m11] [-o arsip.gila] file/folder ...\n\n"
               "  -mN : level memori (default 8 ~ 0,7 GB RAM, 10 ~ 2,4 GB, 11 ~ 4,4 GB).\n"
               "        Level lebih tinggi = rasio lebih baik untuk data besar.\n"
               "        Dekompres butuh RAM yang sama dengan saat kompres.\n");
        pauseIfOwnConsole();
        return 0;
    }
    if (out.empty()) {
        fs::path a = fs::absolute(ins[0]);
        std::string nm = toU8(a.filename()); if (nm.empty()) nm = toU8(a.parent_path().filename());
        out = uniquePath(a.parent_path() / fromU8(nm + ".gila"));
    }
    rc = doCompress(ins, out, level);
#else
    printf("GILA Dekompres v%d\n\n", GILA_VERSION);
    fs::path in, outDir;
    for (size_t i = 0; i < args.size(); ++i) {
        std::string s = toU8(args[i]);
        if (s == "-o" && i + 1 < args.size()) outDir = args[++i];
        else if (in.empty()) in = args[i];
        else outDir = args[i];
    }
    if (in.empty()) {
        printf("Cara pakai:\n"
               "  - Seret file .gila ke ikon gila-dekompres.exe, ATAU\n"
               "  - gila-dekompres arsip.gila [folder_tujuan]\n\n"
               "  Default: diekstrak ke folder bernama sama di samping arsip.\n");
        pauseIfOwnConsole();
        return 0;
    }
    if (outDir.empty()) {
        fs::path a = fs::absolute(in);
        std::string st = toU8(a.stem());
        if (st.empty()) st = "hasil_gila";
        outDir = uniquePath(a.parent_path() / fromU8(st));
    }
    rc = doDecompress(in, outDir);
#endif
    pauseIfOwnConsole();
    return rc;
}
