/* adpcm2_enc: mono int16 LE PCM (stdin, multiple of 304 samples) -> 2-bit ADPCM2 (argv[1]),
   per-chunk decoder states (argv[2], u32 = pred&0xFFFF | sidx<<16 at each chunk start), and the exact
   decoder output as int16 (argv[3], for previews/SNR).  Encoder = beam search (K survivors), decoder
   arithmetic comes straight from ../adpcm.h so the two can never disagree. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include "../adpcm.h"
#define K 32
#define CH 304
int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: adpcm2_enc out.bin states.bin recon.raw < mono.raw\n"); return 1; }
    size_t cap = 1 << 22, n = 0; int16_t *x = malloc(cap * 2);
    for (;;) { if (n == cap) { cap *= 2; x = realloc(x, cap * 2); } size_t r = fread(x + n, 2, cap - n, stdin); if (!r) break; n += r; }
    if (n % CH) { fprintf(stderr, "input not a multiple of %d samples\n", CH); return 2; }
    uint8_t *bk = malloc(n * K);
    int P[K], S[K]; double C[K]; int m = 1; P[0] = 0; S[0] = 0; C[0] = 0;
    typedef struct { double c; int p, s, par, code; } Cand;
    Cand *cd = malloc(sizeof(Cand) * K * 4);
    for (size_t i = 0; i < n; i++) {
        int nc = 0;
        for (int k = 0; k < m; k++) for (int c = 0; c < 4; c++) {
            int p = P[k], s = S[k]; adpcm2_step(c, &p, &s);
            double e = x[i] - p; cd[nc].c = C[k] + e * e; cd[nc].p = p; cd[nc].s = s; cd[nc].par = k; cd[nc].code = c; nc++;
        }
        int nm = 0;
        while (nm < K && nc > 0) {
            int b = 0; for (int j = 1; j < nc; j++) if (cd[j].c < cd[b].c) b = j;
            Cand t = cd[b]; cd[b] = cd[--nc];
            int dup = 0; for (int j = 0; j < nm; j++) if (P[j] == t.p && S[j] == t.s) { dup = 1; break; }
            if (dup) continue;
            P[nm] = t.p; S[nm] = t.s; C[nm] = t.c; bk[i * K + nm] = (uint8_t)(t.par << 2 | t.code); nm++;
        }
        m = nm;
    }
    int b = 0; for (int k = 1; k < m; k++) if (C[k] < C[b]) b = k;
    uint8_t *codes = malloc(n); int k = b;
    for (long i = (long)n - 1; i >= 0; i--) { uint8_t v = bk[i * K + k]; codes[i] = v & 3; k = v >> 2; }
    FILE *fo = fopen(argv[1], "wb"), *fs = fopen(argv[2], "wb"), *fr = fopen(argv[3], "wb");
    int pred = 0, sidx = 0; double se = 0, sx = 0;
    for (size_t c = 0; c < n / CH; c++) {
        uint32_t st = (uint32_t)(pred & 0xFFFF) | ((uint32_t)sidx << 16); fwrite(&st, 4, 1, fs);
        uint8_t out[CH / 4] = {0};
        for (int i = 0; i < CH; i++) {
            int code = codes[c * CH + i]; adpcm2_step(code, &pred, &sidx);
            out[i >> 2] |= (uint8_t)(code << (2 * (i & 3)));
            int16_t rv = (int16_t)((pred >> 8) << 8); fwrite(&rv, 2, 1, fr);
            double e = x[c * CH + i] - rv; se += e * e; sx += (double)x[c * CH + i] * x[c * CH + i];
        }
        fwrite(out, 1, CH / 4, fo);
    }
    fclose(fo); fclose(fs); fclose(fr);
    fprintf(stderr, "chunks %zu  bytes %zu  SNR(8-bit out) %.2f dB\n", n / CH, n / 4, 10 * log10(sx / se));
    return 0;
}
