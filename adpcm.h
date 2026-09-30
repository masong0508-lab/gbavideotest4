/* Shared IMA-style ADPCM decoder. Included by main.c (GBA) and host/test_adpcm.c (PC),
   so the player and the tests always run the exact same code. */
#ifndef ADPCM_H
#define ADPCM_H
typedef unsigned char  u8;
typedef signed char    s8;
typedef unsigned short u16;
typedef unsigned int   u32;

static const short step_tab[89]={7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7847,8631,9494,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767};
static const s8 idx_tab[8] = {-1,-1,-1,-1,2,4,6,8};

/* Decode nbytes of ADPCM (2 samples each, low nibble first) into 8-bit samples. */
static void adpcm_decode(const u8 *p, s8 *out, int nbytes, int *ppred, int *psidx) {
    int pred = *ppred, sidx = *psidx;
    for (int i = 0; i < nbytes; i++) {
        u8 b = p[i];
        for (int k = 0; k < 2; k++) {
            int nib = k ? (b >> 4) : (b & 15);
            int step = step_tab[sidx];
            int diff = step >> 3;
            if (nib & 1) diff += step >> 2;
            if (nib & 2) diff += step >> 1;
            if (nib & 4) diff += step;
            if (nib & 8) pred -= diff; else pred += diff;
            pred -= pred >> 9;                    /* leak: bleeds off any wrong-start offset after a seek */
            if (pred > 32767) pred = 32767;
            if (pred < -32768) pred = -32768;
            sidx += idx_tab[nib & 7];
            if (sidx < 0) sidx = 0;
            if (sidx > 88) sidx = 88;
            out[i * 2 + k] = (s8)(pred >> 8);
        }
    }
    *ppred = pred; *psidx = sidx;
}
/* ---- ADPCM2: 2-bit MONO ADPCM used by the hidden Konami clip (4 samples per byte, lowest bits first). ----
   Code: bit1 = sign, bit0 = "big" (step*1.5) vs "small" (step*0.5).  Big codes grow the step index by
   ADPCM2_GROW, small ones shrink it by 1.  Shares step_tab with the 4-bit codec; same leak, same 8-bit output. */
#define ADPCM2_GROW 3
static inline void adpcm2_step(int code, int *ppred, int *psidx) {
    int step = step_tab[*psidx];
    int diff = (step >> 1) + ((code & 1) ? step : 0);
    int pred = *ppred;
    if (code & 2) pred -= diff; else pred += diff;
    pred -= pred >> 9;
    if (pred > 32767) pred = 32767;
    if (pred < -32768) pred = -32768;
    int s = *psidx + ((code & 1) ? ADPCM2_GROW : -1);
    if (s < 0) s = 0;
    if (s > 88) s = 88;
    *ppred = pred; *psidx = s;
}
static void adpcm2_decode(const u8 *p, s8 *out, int nbytes, int *ppred, int *psidx) {
    int pred = *ppred, sidx = *psidx;
    for (int i = 0; i < nbytes; i++) {
        u8 b = p[i];
        for (int k = 0; k < 4; k++) {
            adpcm2_step((b >> (2 * k)) & 3, &pred, &sidx);
            out[i * 4 + k] = (s8)(pred >> 8);
        }
    }
    *ppred = pred; *psidx = sidx;
}
#endif
