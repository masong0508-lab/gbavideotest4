/* GBA video player - STEREO version with the title MENU.
   Menu: PLAY / CHAPTERS / CONTROLS (+ two hidden button codes on the main menu).
   Plays ONE looping video part: 120x68 @ 5fps (2x scaled, Mode 4) + stereo 4-bit ADPCM audio
   (left -> FIFO A / DMA1, right -> FIFO B / DMA2, both clocked by Timer 0).
   In video: START (or A+Up) = pause/resume, A+Right = fast-forward, A+Left (or A+Down) = rewind, SELECT = back to menu.
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
#define REG_BLDCNT     REG16(0x04000050)
#define REG_BLDALPHA   REG16(0x04000052)
#define REG_KEYINPUT   REG16(0x04000130)
#define IRQ_VECTOR     REG32(0x03007FFC)
#define PALETTE     ((volatile u16*)0x05000000)
#define VRAM_PAGE0  ((volatile u16*)0x06000000)
#define VRAM_PAGE1  ((volatile u16*)0x0600A000)
#define OAM         ((volatile u16*)0x07000000)
#define OBJ_PAL     ((volatile u16*)0x05000200)
#define OBJ_TILES   ((volatile u32*)0x06014000)

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
#define FPS_NUM 5486u               /* video frame = (vblank_tick * 5486) >> 16  (~5 fps) */

/* Everything is embedded in the ROM.  The film comes from the part folder, the menu/secret/hidden-clip
   files from the repo root (the Makefile passes the part folder to the assembler with -Wa,-I). */
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
    ".global menu_bg\nmenu_bg:\n"
    ".incbin \"menu_bg.bin\"\n"
    ".balign 4\n"
    ".global menu_bg6\nmenu_bg6:\n"
    ".incbin \"menu_bg6.bin\"\n"
    ".balign 4\n"
    ".global menu_pal\nmenu_pal:\n"
    ".incbin \"menu_pal.bin\"\n"
    ".balign 4\n"
    ".global secret_bg\nsecret_bg:\n"
    ".incbin \"secret_bg.bin\"\n"
    ".balign 4\n"
    ".global secret_pal\nsecret_pal:\n"
    ".incbin \"secret_pal.bin\"\n"
    ".balign 4\n"
    ".global x_frames_start\nx_frames_start:\n"        /* hidden clip: same formats as the film */
    ".incbin \"extra_frames.bin\"\n"
    ".balign 4\n"
    ".global x_idx_start\nx_idx_start:\n"
    ".incbin \"extra_idx.bin\"\n"
    ".global x_idx_end\nx_idx_end:\n"
    ".balign 4\n"
    ".global x_pal_start\nx_pal_start:\n"
    ".incbin \"extra_palette.bin\"\n"
    ".global x_pal_end\nx_pal_end:\n"
    ".balign 4\n"
    ".global x_audio_start\nx_audio_start:\n"
    ".incbin \"extra_audio.bin\"\n"
    ".global x_audio_end\nx_audio_end:\n"
    ".balign 4\n"
    ".global x_state\nx_state:\n"
    ".incbin \"extra_state.bin\"\n"
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
extern const u16 menu_bg[19200], menu_bg6[19200], menu_pal[256], secret_bg[19200], secret_pal[256];
extern const u8  x_frames_start[];
extern const u32 x_idx_start[], x_idx_end[];
extern const PalSeg x_pal_start[], x_pal_end[];
extern const u8  x_audio_start[], x_audio_end[];
extern const u32 x_state[];

/* One playable video: the film of this ROM part, or the hidden clip. */
typedef struct {
    const u8 *frames;
    const u32 *idx;  u32 nuniq;        /* per unique frame: byte offset into frames (+1 final entry) */
    const PalSeg *pal; u32 npal;
    const u8 *audio; u32 nchunks;
    const u32 *state;
} Stream;
static Stream film, extra;
static void init_streams(void) {
    film.frames = frames_start;  film.idx = frames_idx_start;  film.nuniq = (u32)(frames_idx_end - frames_idx_start) - 1;
    film.pal = palette_data;     film.npal = (u32)(palette_end - palette_data);
    film.audio = audio_start;    film.nchunks = (u32)(audio_end - audio_start) / CHUNK_BYTES;
    film.state = audio_state;
    extra.frames = x_frames_start; extra.idx = x_idx_start;   extra.nuniq = (u32)(x_idx_end - x_idx_start) - 1;
    extra.pal = x_pal_start;       extra.npal = (u32)(x_pal_end - x_pal_start);
    extra.audio = x_audio_start;   extra.nchunks = (u32)(x_audio_end - x_audio_start) / CHUNK_BYTES;
    extra.state = x_state;
}

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

/* ================= menu (from the template) ================= */
static const char font_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ123456";
static const u8 font5x7[32][5] = {
{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
{0x7F,0x41,0x41,0x41,0x3E},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
{0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
{0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
{0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
{0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},
{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
{0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30}   /* 4 5 6 */
};

static void put(int x, int y, u8 c) {
    if ((unsigned)x >= 240 || (unsigned)y >= 160) return;
    volatile u16 *p = VRAM_PAGE0 + ((y * 240 + x) >> 1);
    u16 v = *p;
    *p = (x & 1) ? (u16)((v & 0xFF) | (c << 8)) : (u16)((v & 0xFF00) | c);
}

static const u8 *glyph(char ch) {
    for (int i = 0; font_chars[i]; i++) if (font_chars[i] == ch) return font5x7[i];
    return 0;
}

static int text_w(const char *s) { int n = 0; while (*s++) n++; return n * 7 - 1; }

/* bold 5x7 text: colour 255 (white) with colour 254 (black) shadow */
static void text(int x, int y, const char *s) {
    for (int pass = 0; pass < 2; pass++) {
        int cx = x;
        for (const char *p = s; *p; p++, cx += 7) {
            const u8 *g = glyph(*p);
            if (!g) continue;
            for (int c = 0; c < 5; c++)
                for (int r = 0; r < 7; r++)
                    if ((g[c] >> r) & 1) {
                        if (pass == 0) { put(cx + c + 1, y + r + 1, 254); put(cx + c + 2, y + r + 1, 254); }
                        else           { put(cx + c, y + r, 255);         put(cx + c + 1, y + r, 255); }
                    }
        }
    }
}

static void menu_setup(const u16 *bg) {
    REG_IME = 0;
    for (int i = 0; i < 256; i++) PALETTE[i] = menu_pal[i];
    REG_DISPCNT = 4 | (1 << 10);
    for (int i = 0; i < 19200; i++) VRAM_PAGE0[i] = bg[i];
    for (int i = 0; i < 128; i++) OAM[i * 4] = 0x200;      /* hide all sprites */
}

/* 64x32 rounded highlight sprite (4bpp, black), alpha-blended over the background */
static void make_highlight(void) {
    OBJ_PAL[1] = 0;                             /* black core */
    OBJ_PAL[2] = 0;                             /* black rim */
    for (int ty = 0; ty < 4; ty++)
        for (int tx = 0; tx < 8; tx++)
            for (int r = 0; r < 8; r++) {
                u32 w = 0;
                for (int k = 0; k < 8; k++) {
                    int X = tx * 8 + k, Y = ty * 8 + r;
                    int dx = X < 12 ? 12 - X : (X > 51 ? X - 51 : 0);
                    int dy = Y < 12 ? 12 - Y : (Y > 19 ? Y - 19 : 0);
                    int d = dx * dx + dy * dy;
                    u32 px = d > 144 ? 0 : (d > 81 ? 2 : 1);
                    w |= px << (4 * k);
                }
                OBJ_TILES[(ty * 8 + tx) * 8 + r] = w;
            }
    REG_BLDCNT = 0x0450;                        /* OBJ 1st target, alpha, BG2 2nd target */
    REG_BLDALPHA = 5 | (11 << 8);
}

#define ROW_Y(i) (15 + 23 * (i))                /* text row centres from the layout guide */
#define COL_CX 192

/* Secret screen (Konami code on the main menu). Same format as the menu
   background: 240x160 8-bit, palette idx 254 = black, 255 = white.
   Shows the image until any button is pressed, then returns to the menu.
   Put your own sound/animation here if you want one. */
static void secret(void) {
    REG_IME = 0;
    for (int i = 0; i < 256; i++) PALETTE[i] = secret_pal[i];
    REG_DISPCNT = 4 | (1 << 10);
    for (int i = 0; i < 19200; i++) VRAM_PAGE0[i] = secret_bg[i];
    for (int i = 0; i < 128; i++) OAM[i * 4] = 0x200;
    text(120 - text_w("SECRET") / 2, 76, "SECRET");   /* remove this line for a text-free screen */

    u16 prev = (u16)(~REG_KEYINPUT & 0x3FF);
    for (;;) {
        wait_vb();
        u16 k = (u16)(~REG_KEYINPUT & 0x3FF);
        u16 hit = k & ~prev;
        prev = k;
        if (hit) return;
    }
}

/* returns chosen index, or -1 for B (only when allow_back) */
static int menu(const char *const *items, int n, int allow_back) {
    menu_setup(n > 3 ? menu_bg6 : menu_bg);     /* 3 pills for the main menu, 6 for the chapter list */
    for (int i = 0; i < n; i++) text(COL_CX - text_w(items[i]) / 2, ROW_Y(i) - 3, items[i]);
    make_highlight();
    REG_DISPCNT = 4 | (1 << 10) | (1 << 6) | (1 << 12);
    int sel = 0;
    static const u16 konami[10]  = { 0x40, 0x40, 0x80, 0x80, 0x20, 0x10, 0x20, 0x10, 0x02, 0x01 };
    /* Altered code: D D U U L R L R B A, triggers the easter-egg clip */
    static const u16 konami2[10] = { 0x80, 0x80, 0x40, 0x40, 0x20, 0x10, 0x20, 0x10, 0x02, 0x01 };
    int ki = 0, ki2 = 0;
    u16 prev = (u16)(~REG_KEYINPUT & 0x3FF);
    for (;;) {
        wait_vb();
        OAM[0] = (u16)(((ROW_Y(sel) - 16) & 0xFF) | (1 << 10) | (1 << 14));
        OAM[1] = (u16)(((COL_CX - 32) & 0x1FF) | (3 << 14));
        OAM[2] = 512;
        u16 k = (u16)(~REG_KEYINPUT & 0x3FF);
        u16 hit = k & ~prev;
        prev = k;
        if (!allow_back && hit) {                            /* Konami: U U D D L R L R B A */
            if (hit == konami[ki]) ki++;
            else ki = (hit == konami[0]) ? 1 : 0;
            if (ki == 10) { secret(); return -2; }

            if (hit == konami2[ki2]) ki2++;
            else ki2 = (hit == konami2[0]) ? 1 : 0;
            if (ki2 == 10) { ki2 = 0; return -3; }
        }
        if (hit & 0x40) sel = (sel + n - 1) % n;             /* up */
        if (hit & 0x80) sel = (sel + 1) % n;                 /* down */
        if (hit & 0x09) return sel;                          /* A / START */
        if ((hit & 0x02) && allow_back) return -1;           /* B */
    }
}

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


/* ---- CONTROLS screen ---- */
static void controls(void) {
    menu_setup(menu_bg);
    for (int y = 10; y < 150; y++) for (int x = 10; x < 230; x++) put(x, y, 254);
    text(20, 16, "CONTROLS");
    text(20, 34, "UP DOWN   MOVE");
    text(20, 48, "A OR START   SELECT");
    text(20, 62, "B   BACK");
    text(20, 80, "IN VIDEO");
    text(20, 94, "START OR A UP   PAUSE");
    text(20, 108, "A RIGHT   FAST FORWARD");
    text(20, 122, "A LEFT OR DOWN   REWIND");
    text(20, 136, "SELECT   MENU");
    u16 prev = (u16)(~REG_KEYINPUT & 0x3FF);
    for (;;) {
        wait_vb();
        u16 k = (u16)(~REG_KEYINPUT & 0x3FF);
        u16 hit = k & ~prev;
        prev = k;
        if (hit & 0x0B) return;
    }
}

#define SEEK_STEP 4                         /* ticks (vblanks) per frame while seeking = 4x speed */

#define NCHAPTERS 6
/* Chapters: PART 1..6 = the start of each sixth of this ROM's video (audio chunk aligned). */
static u32 chapter_tick(const Stream *S, int p) {
    u32 count = S->nuniq * 2;
    u32 f = count * (u32)p / NCHAPTERS;
    u32 t = ((f << 16) + (FPS_NUM - 1)) / FPS_NUM;
    t &= ~(u32)(VB_PER_CHUNK - 1);
    if (t / VB_PER_CHUNK >= S->nchunks) t = 0;
    return t;
}

/* Plays one Stream from tick `st` until SELECT is pressed (then returns to the menu). */
static void play_stream(const Stream *S, u32 st) {
    const u32 count = S->nuniq * 2;         /* idx has one entry per unique frame; each is shown for 2 steps */
    u32 maxpos = (count << 16) / FPS_NUM;
    if (maxpos > S->nchunks * VB_PER_CHUNK - VB_PER_CHUNK) maxpos = S->nchunks * VB_PER_CHUNK - VB_PER_CHUNK;
    maxpos &= ~(u32)(VB_PER_CHUNK - 1);
    st &= ~(u32)(VB_PER_CHUNK - 1);
    if (st > maxpos) st = 0;

    REG_IME = 0;
    REG_BLDCNT = 0;                                         /* the menu's highlight blend is off */
    for (int i = 0; i < 128; i++) OAM[i * 4] = 0x200;       /* hide the menu's sprites */
    for (int i = 0; i < 256; i++) PALETTE[i] = 0;
    for (int i = 0; i < 19200; i++) { VRAM_PAGE0[i] = 0; VRAM_PAGE1[i] = 0; }
    REG_DISPCNT = 4 | (1 << 10);                            /* Mode 4, page 0 */

    unsigned f0 = (st * FPS_NUM) >> 16;
    if (f0 >= count) f0 = count - 1;
    u32 seg = 0;
    while (seg + 1 < S->npal && S->pal[seg + 1].first <= f0) seg++;
    for (int i = 0; i < 16; i++) PALETTE[i + 1] = S->pal[seg].col[i];
    u32 pend_seg = seg;

    start_audio(st, S->audio, S->nchunks, S->state);

    u32 last = st, pos = st;
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

        if (hit & 4) break;                 /* SELECT: back to the menu */
        if (hit & 8) paused ^= 1;           /* START: pause / resume */
        int a = k & 1;
        if (a && (hit & 0x40)) paused ^= 1; /* A + Up: pause / resume */
        else if ((hit & 1) && (k & 0x40)) paused ^= 1;
        int nm = paused ? 1 : 0;
        if (a && (k & 0x10)) nm = 2;                        /* A + Right: fast-forward */
        else if (a && (k & 0xA0)) nm = 3;                   /* A + Left or A + Down: rewind */

        if (nm != mode) {
            if (mode == 0) { pos = t > maxpos ? maxpos : t; stop_audio(); }
            if (nm == 0) {                  /* resume: re-sync audio to the video position */
                u32 s = pos & ~(u32)(VB_PER_CHUNK - 1);
                start_audio(s, S->audio, S->nchunks, S->state);
                last = s; t = s;
            }
            mode = nm;
        }

        if (pending) {
            page ^= 1;
            if (pend_seg != seg) {                 /* new scene: swap the 16 colours together with the page flip */
                seg = pend_seg;
                for (int i = 0; i < 16; i++) PALETTE[i + 1] = S->pal[seg].col[i];
            }
            REG_DISPCNT = 4 | (1 << 10) | (page << 4);
            pending = 0;
        }
        unsigned f = (t * FPS_NUM) >> 16;
        if (f >= count) f = count - 1;
        if ((int)(f >> 1) != (drawn < 0 ? -1 : (drawn >> 1))) {   /* same unique frame -> nothing to redraw */
            while (pend_seg + 1 < S->npal && S->pal[pend_seg + 1].first <= f) pend_seg++;
            while (pend_seg > 0 && S->pal[pend_seg].first > f) pend_seg--;
            decode_frame(S->frames, S->idx[f >> 1], S->idx[(f >> 1) + 1]);
            draw_frame(frame_buf, page ? VRAM_PAGE0 : VRAM_PAGE1);
            drawn = (int)f;
            pending = 1;
        }
    }

    stop_audio();
    REG_DISPCNT = 4 | (1 << 10);            /* back to page 0 */
}

int main(void) {
    static const char *const main_items[3] = { "PLAY", "CHAPTERS", "CONTROLS" };
    static const char *const chap_items[NCHAPTERS] = { "PART 1", "PART 2", "PART 3", "PART 4", "PART 5", "PART 6" };
    init_streams();
    for (;;) {
        int a = menu(main_items, 3, 0);
        if (a == -2) continue;                                  /* secret screen was shown */
        if (a == -3) { play_stream(&extra, 0); continue; }      /* D D U U L R L R B A: hidden clip */
        if (a == 0) play_stream(&film, 0);
        else if (a == 1) { int c = menu(chap_items, NCHAPTERS, 1); if (c >= 0) play_stream(&film, chapter_tick(&film, c)); }
        else controls();
    }
}
