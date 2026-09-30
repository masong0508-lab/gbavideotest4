/* RLE frame decoder shared by main.c (GBA) and host/test_clip.c (PC).
   Stream = (run,value) byte pairs, run 1..255.  Extension used by the Konami clip: run == 0 means
   "repeat the previous 120-pixel row" (value byte ignored), so the clip is stored at half vertical
   resolution.  The film never emits run 0, so it decodes exactly as before. */
#ifndef RLE_H
#define RLE_H
static void rle_decode_frame(const u8 *comp, u32 off, u32 end, u8 *frame, int frame_bytes, int row_w) {
    const u8 *p = comp + off, *stop = comp + end;
    u8 *out = frame, *out_end = frame + frame_bytes;
    while (p < stop && out < out_end) {
        u8 run = *p++, val = *p++;
        if (run == 0) {                                   /* repeat previous row */
            if (out - frame >= row_w)
                for (int i = 0; i < row_w && out < out_end; i++, out++) *out = *(out - row_w);
            continue;
        }
        for (u8 i = 0; i < run && out < out_end; i++) *out++ = val;
    }
}
#endif
