/* clip_rle: lossy row-pair RLE for the Konami clip.
   argv: pal.rgb(16*3 bytes) target_bytes_per_frame out.bin sizes.txt [carry]
   stdin: frames of 120x34 palette indices 0..15.  Output per frame: for each of the 34 rows the row's
   (run,value) pairs followed by a (0,0) "repeat previous row" pair -> a 120x68 picture. Values are +1 (1..16).
   A byte budget per frame is met by raising the colour-merge threshold T (runs absorb near-identical colours);
   'carry' lets easy frames donate bytes to hard ones. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define W 120
#define R 34
#define FB (W*R)
static int D[16][16];
static int enc(const uint8_t *px, int T, uint8_t *out) {
    int n = 0;
    for (int y = 0; y < R; y++) {
        const uint8_t *r = px + y * W; int c = r[0], run = 1;
        for (int x = 1; x < W; x++) {
            int p = r[x];
            if (run < 255 && (p == c || D[p][c] <= T)) run++;
            else { out[n++] = run; out[n++] = c + 1; c = p; run = 1; }
        }
        out[n++] = run; out[n++] = c + 1;
        out[n++] = 0; out[n++] = 0;
    }
    return n;
}
int main(int argc, char **argv) {
    FILE *fp = fopen(argv[1], "rb"); uint8_t pal[48] = {0}; if (fread(pal, 3, 16, fp) == 0) return 1; fclose(fp);
    double target = atof(argv[2]); FILE *fo = fopen(argv[3], "wb"), *fs = fopen(argv[4], "w");
    for (int a = 0; a < 16; a++) for (int b = 0; b < 16; b++) {
        int dr = pal[a*3]-pal[b*3], dg = pal[a*3+1]-pal[b*3+1], db = pal[a*3+2]-pal[b*3+2];
        D[a][b] = 2*dr*dr + 4*dg*dg + 3*db*db;
    }
    static uint8_t px[FB], out[FB * 4 + 64]; double carry = argc > 5 ? atof(argv[5]) : 0;
    static const int Ts[] = {0,30,60,100,160,240,340,480,680,950,1300,1800,2500,3500,5000,8000,14000,30000,1000000};
    int nT = sizeof(Ts) / sizeof(int);
    while (fread(px, 1, FB, stdin) == FB) {
        double cap = target + (carry > 0 ? (carry < target * 2 ? carry : target * 2) : carry * 0.5);
        if (cap < target * 0.5) cap = target * 0.5;
        int n = 0, k;
        for (k = 0; k < nT; k++) { n = enc(px, Ts[k], out); if (n <= cap) break; }
        if (k == nT) { k = nT - 1; n = enc(px, Ts[k], out); }
        fwrite(out, 1, n, fo); fprintf(fs, "%d %d\n", n, Ts[k]); carry += target - n;
    }
    fprintf(stderr, "%f\n", carry); return 0;
}
