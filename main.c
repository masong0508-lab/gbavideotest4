/* GBA video player - STEREO version.
   Plays ONE looping video part: 120x68 @ 5fps (2x scaled, Mode 4) + stereo 4-bit ADPCM audio
   (left -> FIFO A / DMA1, right -> FIFO B / DMA2, both clocked by Timer 0).
   START = pause/resume, A+Right = fast-forward, A+Left = rewind.
   All video/audio data lives in the .bin files; see README.md and tools/gbatool.py. */
#include "adpcm.h"

#define REG16(a) (*(volatile u16*)(a))
#define REG32(a) (*(volatile u32*)(a))
#define REG_DISPCNT    REG16(0x04000000)
#define REG_DISPSTAT   REG16(0x04000004)
#define REG_SOUNDCNT_L REG16(0x04000080)
#define REG_SOUNDCNT_H REG16(0x04000082)
#define REG_SOUNDCNT_X REG16(0x04000084)
#define REG_TM0CNT_L   REG16(0x04000100)
#define REG_TM0CNT_H   REG16(0x04000102)
#define REG_DMA1SAD    REG32(0x040000BC)
#define REG_DMA1DAD    REG32(0x040000C0)
#define REG_DMA1CNT    REG32(0x040000C4)
#define REG_DMA2SAD    REG32(0x040000C8)
#define REG_DMA2DAD    REG32(0x040000CC)
#define REG_DMA2CNT    REG32(0x040000D0)
#define REG_IE         REG16(0x04000200)
#define REG_IF         REG16(0x04000202)
#define REG_IME        REG16(0x04000208)
#define REG_VCOUNT     REG16(0x04000006)
#define REG_KEYINPUT   REG16(0x04000130)
#define IRQ_VECTOR     REG32(0x03007FFC)
#define PALETTE     ((volatile u16*)0x05000000)
#define VRAM_PAGE0  ((volatile u16*)0x06000000)
#define VRAM_PAGE1  ((volatile u16*)0x0600A000)

#define VID_W 120
#define VID_H 68
#define Y_OFFSET 12                 /* (160 - 68*2) / 2 */
#define FRAME_BYTES (VID_W * VID_H)

/* Audio: 4539.25 Hz (timer period 3696 cycles). One 304-sample chunk plays
   for exactly 4 vblanks (3696 * 304 = 4 * 280896 cycles). */
#define SAMPLES_PER_CHUNK 304
/* audio.bin = per chunk: 152 bytes left ADPCM, then 152 bytes right ADPCM */
#define VB_PER_CHUNK 4
#define TIMER_PERIOD 3696
#define CHUNK_BYTES SAMPLES_PER_CHUNK
#define NCHUNKS ((u32)(audio_end - audio_start) / CHUNK_BYTES)

/* Everything is embedded in the ROM */
__asm__(
    ".section .rodata\n"
    ".balign 4\n"
    ".global frames_start\nframes_start:\n"
    ".incbin \"frames1a.bin\"\n"         /* every piece stays well under GitHub's 25 MB upload limit; */
    ".incbin \"frames1b.bin\"\n"         /* .incbin lines are back-to-back, so the ROM bytes are identical */
    ".incbin \"frames2.bin\"\n"
    ".global frames_end\nframes_end:\n"
    ".balign 4\n"
    ".global frames_idx_start\nframes_idx_start:\n"
    ".incbin \"frames_idx.bin\"\n"
    ".global frames_idx_end\nframes_idx_end:\n"
    ".balign 4\n"
    ".global palette_data\npalette_data:\n"
    ".incbin \"palette.bin\"\n"
    ".global palette_end\npalette_end:\n"
    ".balign 4\n"
    ".global audio_start\naudio_start:\n"
    ".incbin \"audio_a.bin\"\n"
    ".incbin \"audio_b.bin\"\n"
    ".global audio_end\naudio_end:\n"
    ".balign 4\n"
    ".global audio_state\naudio_state:\n"
    ".incbin \"audio_state.bin\"\n"
    ".balign 4\n"
    ".text\n"
);
extern const u8  frames_start[], frames_end[];
extern const u32 frames_idx_start[], frames_idx_end[];
/* palette.bin: per scene segment { u32 first_frame; u16 colour[16]; } (36 bytes). Colours go to palette
   entries 1..16; entry 0 stays black (letterbox bars). Video pixel values are already 1..16. */
typedef struct { u32 first; u16 col[16]; } PalSeg;
extern const PalSeg palette_data[], palette_end[];
extern const u8  audio_start[], audio_end[];
extern const u32 audio_state[];   /* per chunk, [L,R]: low 16 bits = predictor (s16), bits 16-23 = step index */

/* ---- audio state ---- */
static s8 abuf[2][2][SAMPLES_PER_CHUNK] __attribute__((aligned(4)));   /* [buffer][channel][sample] */
static volatile u32 tick;          /* vblank counter: drives video position, reset when the loop restarts */
static volatile u32 achunk_ctr;    /* vblanks since the last audio chunk boundary */
static volatile u32 started;       /* chunks started so far in this loop */
static volatile u32 play_idx;      /* buffer to start at the next chunk boundary */
static volatile u32 g_vb_per_chunk;
static volatile u16 g_timer_reload;

static const u8 *ap;               /* start of the next chunk in audio.bin */
static int pred[2], sidx[2];
static u32 dec;
static const u8 *g_audio_base;
static u32 g_nchunks;

static void decode_chunk(int buf) {
    if (dec == g_nchunks) { dec = 0; ap = g_audio_base; pred[0] = pred[1] = 0; sidx[0] = sidx[1] = 0; }
    adpcm_decode(ap,                       abuf[buf][0], SAMPLES_PER_CHUNK / 2, &pred[0], &sidx[0]);
    adpcm_decode(ap + SAMPLES_PER_CHUNK/2, abuf[buf][1], SAMPLES_PER_CHUNK / 2, &pred[1], &sidx[1]);
    ap += CHUNK_BYTES;
    dec++;
}

/* VBlank interrupt: called by the BIOS in ARM state */
__attribute__((target("arm")))
static void irq_handler(void) {
    u16 flags = REG_IF;
    if (flags & 1) {
        tick++;
        achunk_ctr++;
        if (achunk_ctr >= g_vb_per_chunk) {           /* one audio-chunk boundary */
            achunk_ctr = 0;
            if (started == g_nchunks) { started = 0; tick = 0; }   /* loop: resync video too */
            REG_DMA1CNT = 0;
            REG_DMA2CNT = 0;
            REG_TM0CNT_H = 0;
            REG_DMA1SAD = (u32)abuf[play_idx][0];
            REG_DMA1DAD = 0x040000A0;         /* FIFO A = left */
            REG_DMA1CNT = 0xB6400001;         /* fifo mode, 32-bit, repeat, enable */
            REG_DMA2SAD = (u32)abuf[play_idx][1];
            REG_DMA2DAD = 0x040000A4;         /* FIFO B = right */
            REG_DMA2CNT = 0xB6400001;
            REG_TM0CNT_L = g_timer_reload;
            REG_TM0CNT_H = 0x80;
            play_idx ^= 1;
            started++;
            decode_chunk(play_idx);     /* refill right here: never depends on the main loop being free */
        }
    }
    REG_IF = flags;
}

/* Frames are stored RLE-compressed (run,value byte pairs; run 1-255) since the raw
   8160 bytes/frame would not fit a long video in 32 MB. idx[f]..idx[f+1] bounds the
   compressed bytes for frame f in the stream starting at comp. */
static u8 frame_buf[FRAME_BYTES];

static void decode_frame(const u8 *comp, u32 off, u32 end) {
    const u8 *p = comp + off, *stop = comp + end;
    u8 *out = frame_buf, *out_end = frame_buf + FRAME_BYTES;
    while (p < stop && out < out_end) {
        u8 run = *p++, val = *p++;
        for (u8 i = 0; i < run && out < out_end; i++) *out++ = val;
    }
}

/* Draw a 120x68 frame doubled to 240x136 into Mode 4 VRAM, centred. */
static void draw_frame(const u8 *src, volatile u16 *dst) {
    for (int y = 0; y < VID_H; y++) {
        volatile u32 *row0 = (volatile u32 *)(dst + (Y_OFFSET + y * 2) * 120);
        volatile u32 *row1 = row0 + 60;
        const u8 *s = src + y * VID_W;
        for (int x = 0; x < VID_W; x += 2) {
            u32 a = s[x], b = s[x + 1];
            u32 w = a | (a << 8) | (b << 16) | (b << 24);     /* 2 source pixels -> 4 VRAM bytes */
            row0[x >> 1] = w;
            row1[x >> 1] = w;
        }
    }
}

static void wait_vb(void) { while (REG_VCOUNT >= 160) {} while (REG_VCOUNT < 160) {} }

/* ================= playback ================= */
static void start_audio(u32 st, const u8 *audio_base, u32 nchunks, const u32 *states) {
    REG_IME = 0;
    REG_SOUNDCNT_X = 0x80;
    /* DMA A+B 100% vol, A -> left only, B -> right only, both on timer 0, reset both FIFOs */
    REG_SOUNDCNT_H = 0x9A0C;
    g_audio_base = audio_base;
    g_nchunks = nchunks;
    g_vb_per_chunk = VB_PER_CHUNK;
    g_timer_reload = (u16)(65536 - TIMER_PERIOD);
    u32 c = st / g_vb_per_chunk;
    dec = c; ap = audio_base + c * CHUNK_BYTES;
    /* ADPCM only decodes correctly from the exact state it had at that point in the stream. */
    for (int ch = 0; ch < 2; ch++) {
        u32 sv = states[c * 2 + ch]; pred[ch] = (short)(sv & 0xFFFF); sidx[ch] = (int)((sv >> 16) & 0xFF);
    }
    play_idx = 0; started = c; tick = st; achunk_ctr = g_vb_per_chunk - 1;
    decode_chunk(0);
    IRQ_VECTOR = (u32)irq_handler;
    REG_DISPSTAT = 8;
    REG_IF = 0xFFFF;
    REG_IE = 1;
    REG_IME = 1;
}

static void stop_audio(void) {
    REG_IME = 0;
    REG_IE = 0;
    REG_DISPSTAT = 0;
    REG_DMA1CNT = 0;
    REG_DMA2CNT = 0;
    REG_TM0CNT_H = 0;
    REG_SOUNDCNT_H = 0x8800;                /* reset both FIFOs, silence */
    REG_IF = 0xFFFF;
}

#define SEEK_STEP 4                         /* ticks (vblanks) per frame while seeking = 4x speed */

int main(void) {
    unsigned count = ((unsigned)(frames_idx_end - frames_idx_start) - 1) * 2;   /* idx has one entry per unique frame; each is shown for 2 steps */
    const u32 nseg = (u32)(palette_end - palette_data);
    const u32 fps_num = 5486u;
    u32 maxpos = ((u32)count << 16) / fps_num;
    if (maxpos > NCHUNKS * VB_PER_CHUNK - VB_PER_CHUNK) maxpos = NCHUNKS * VB_PER_CHUNK - VB_PER_CHUNK;
    maxpos &= ~(u32)(VB_PER_CHUNK - 1);

    for (int i = 0; i < 256; i++) PALETTE[i] = 0;
    u32 seg = 0;
    for (int i = 0; i < 16; i++) PALETTE[i + 1] = palette_data[0].col[i];
    u32 pend_seg = 0;
    for (int i = 0; i < 19200; i++) VRAM_PAGE0[i] = 0;
    REG_DISPCNT = 4 | (1 << 10);            /* Mode 4, page 0 */

    start_audio(0, audio_start, NCHUNKS, audio_state);

    u32 last = 0, pos = 0;
    int mode = 0;                           /* 0 play, 1 pause, 2 fast-forward, 3 rewind */
    int paused = 0;
    int drawn = -1, pending = 0, page = 0;
    u16 prev = (u16)(~REG_KEYINPUT & 0x3FF);

    for (;;) {
        u32 t;
        if (mode == 0) {
            while (tick == last) __asm__ volatile("swi 0x02" ::: "r0","r1","r2","r3","memory");   /* BIOS Halt */
            t = tick;
            last = t;
        } else {
            wait_vb();
            if (mode == 2) { pos += SEEK_STEP; if (pos > maxpos) pos = maxpos; }
            if (mode == 3) { pos = pos > SEEK_STEP ? pos - SEEK_STEP : 0; }
            t = pos;
        }

        u16 k = (u16)(~REG_KEYINPUT & 0x3FF);
        u16 hit = k & ~prev;
        prev = k;

        if (hit & 8) paused ^= 1;           /* START: pause / resume */
        int a = k & 1;
        int nm = paused ? 1 : 0;
        if (a && (k & 0x10)) nm = 2;        /* A + Right: fast-forward */
        else if (a && (k & 0x20)) nm = 3;   /* A + Left: rewind */

        if (nm != mode) {
            if (mode == 0) { pos = t > maxpos ? maxpos : t; stop_audio(); }
            if (nm == 0) {                  /* resume: re-sync audio to the video position */
                u32 s = pos & ~(u32)(VB_PER_CHUNK - 1);
                start_audio(s, audio_start, NCHUNKS, audio_state);
                last = s; t = s;
            }
            mode = nm;
        }

        if (pending) {
            page ^= 1;
            if (pend_seg != seg) {                 /* new scene: swap the 16 colours together with the page flip */
                seg = pend_seg;
                for (int i = 0; i < 16; i++) PALETTE[i + 1] = palette_data[seg].col[i];
            }
            REG_DISPCNT = 4 | (1 << 10) | (page << 4);
            pending = 0;
        }
        unsigned f = (t * fps_num) >> 16;
        if (f >= count) f = count - 1;
        if ((int)(f >> 1) != (drawn < 0 ? -1 : (drawn >> 1))) {   /* same unique frame -> nothing to redraw */
            while (pend_seg + 1 < nseg && palette_data[pend_seg + 1].first <= f) pend_seg++;
            while (pend_seg > 0 && palette_data[pend_seg].first > f) pend_seg--;
            decode_frame(frames_start, frames_idx_start[f >> 1], frames_idx_start[(f >> 1) + 1]);
            draw_frame(frame_buf, page ? VRAM_PAGE0 : VRAM_PAGE1);
            drawn = (int)f;
            pending = 1;
        }
    }
}
