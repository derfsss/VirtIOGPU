/*
 * chip_flush.c -- Framebuffer flush and pixel format conversion.
 *
 * Converts P96 board_mem (CLUT/TrueColor) -> GPU fb_mem (32bpp X8R8G8B8),
 * then transfers dirty rectangles to the VirtIO GPU via TRANSFER_TO_HOST_2D
 * and RESOURCE_FLUSH.  Also contains the periodic flush task.
 *
 * GPU format B8G8R8X8_UNORM: memory bytes [BB, GG, RR, XX].
 * Chosen because on LE-host QEMU it maps to PIXMAN_x8r8g8b8
 * (= Cairo ARGB32), supported correctly by both the pixman/SDL2
 * 2D backend and the SDL-GL backend.  Previously used X8R8G8B8 (=4)
 * worked for gl=off but caused R/B swap with gl=on.
 *
 * Conversion target on PPC BE (byte 0 = MSB):
 *   dst uint32 = (B << 24) | (G << 16) | (R << 8) | X
 *   stored as bytes [BB, GG, RR, XX].
 *
 * Per-format source layouts: see individual conv_row_* below.  The
 * CLUT palette is pre-composed in this layout in chip_SetColorArray.
 */

#include "chip/chip_state.h"
#include <devices/timer.h>
#include <interfaces/timer.h>

/* -----------------------------------------------------------------------
 * chip_prof_end -- record a profiling sample and log per policy.
 *
 * Lives in chip_flush.c because that's where the ITimer pointer is
 * managed.  See struct ChipProf in chip_state.h for the full design.
 * ----------------------------------------------------------------------- */
void chip_prof_end(struct ChipGPUState *gs, struct ChipProf *p,
                    uint32 start_token, uint32 bytes)
{
    if (!gs || !p || start_token == 0 || !gs->ITimer || !gs->eclock_freq)
        return;

    struct EClockVal ev;
    gs->ITimer->ReadEClock(&ev);
    /* start_token is ev_lo+1 from chip_prof_begin; undo the offset.
     * Unsigned subtraction handles a single 32-bit wrap. */
    uint32 start_lo = start_token - 1u;
    uint32 delta_ticks = ev.ev_lo - start_lo;

    /* Convert ticks -> microseconds.  At a 1 MHz EClock and refresh-class
     * samples this fits comfortably in 32 bits; for safety compute via
     * uint64 then narrow.  Skip the divide if delta is zero. */
    uint32 delta_us;
    if (delta_ticks == 0) {
        delta_us = 0;
    } else {
        uint64 tmp = (uint64)delta_ticks * 1000000ULL;
        uint64 us  = tmp / (uint64)gs->eclock_freq;
        delta_us   = (us > 0xFFFFFFFFULL) ? 0xFFFFFFFFu : (uint32)us;
    }

    /* Update accumulators.  Race-tolerant: stat_-style increments may
     * lose the occasional sample but the order-of-magnitude is fine. */
    p->calls++;

    /* sum_us = sum_us + delta_us  (64-bit add via lo/hi pair) */
    uint32 old_lo = p->sum_us_lo;
    uint32 new_lo = old_lo + delta_us;
    p->sum_us_lo  = new_lo;
    if (new_lo < old_lo) p->sum_us_hi++;     /* carry */

    /* sum_bytes += bytes */
    if (bytes) {
        uint32 b_old_lo = p->sum_bytes_lo;
        uint32 b_new_lo = b_old_lo + bytes;
        p->sum_bytes_lo = b_new_lo;
        if (b_new_lo < b_old_lo) p->sum_bytes_hi++;
    }

    /* Track worst-case sample. */
    if (delta_us > p->max_us) p->max_us = delta_us;

    /* First-sample one-shot. */
    if (!p->logged_first) {
        p->logged_first = 1;
        if (bytes) {
            /* Compute MB/s = bytes/us.  Avoid divide-by-zero. */
            uint32 mbs = (delta_us > 0)
                ? (uint32)(((uint64)bytes * 1000000ULL) / ((uint64)delta_us * 1024ULL * 1024ULL))
                : 0;
            DCHIP("PROF[%s]: first call elapsed=%luus bytes=%lu rate=%luMB/s",
                  p->name, (ULONG)delta_us, (ULONG)bytes, (ULONG)mbs);
        } else {
            DCHIP("PROF[%s]: first call elapsed=%luus",
                  p->name, (ULONG)delta_us);
        }
    }

    /* Periodic rolling summary. */
    if (p->summary_interval && p->calls >= p->next_summary_at) {
        p->next_summary_at = p->calls + p->summary_interval;

        /* Recover totals as 64-bit. */
        uint64 sum_us = ((uint64)p->sum_us_hi << 32) | (uint64)p->sum_us_lo;
        uint64 sum_b  = ((uint64)p->sum_bytes_hi << 32) | (uint64)p->sum_bytes_lo;
        uint32 calls  = p->calls;
        uint32 avg_us = calls ? (uint32)(sum_us / (uint64)calls) : 0;
        if (sum_b) {
            uint32 mbs = (sum_us > 0)
                ? (uint32)((sum_b * 1000000ULL) / (sum_us * 1024ULL * 1024ULL))
                : 0;
            DCHIP("PROF[%s]: calls=%lu avg=%luus max=%luus total=%lubytes rate=%luMB/s",
                  p->name, (ULONG)calls, (ULONG)avg_us, (ULONG)p->max_us,
                  (ULONG)sum_b, (ULONG)mbs);
        } else {
            DCHIP("PROF[%s]: calls=%lu avg=%luus max=%luus",
                  p->name, (ULONG)calls, (ULONG)avg_us, (ULONG)p->max_us);
        }
    }

    /* Outlier detection: a single sample more than 4x the running
     * average flags as a stutter spike (logged at most once per bucket
     * per session to avoid noise). */
    if (p->calls > 16) {
        uint64 sum_us  = ((uint64)p->sum_us_hi << 32) | (uint64)p->sum_us_lo;
        uint32 avg_us  = (uint32)(sum_us / (uint64)p->calls);
        if (avg_us > 0 && delta_us > avg_us * 4) {
            static volatile uint32 outlier_warned_mask = 0;
            /* Use the low byte of the bucket-name pointer as a poor-
             * man's hash to bucket the warned-once tracking.  Up to 32
             * distinct buckets get coverage; collisions just mean a
             * second bucket suppresses its own outlier log -- acceptable. */
            uint32 bit = 1u << ((((ULONG)(void *)p->name) >> 2) & 0x1F);
            if (!(outlier_warned_mask & bit)) {
                outlier_warned_mask |= bit;
                DCHIP("PROF[%s]: OUTLIER %luus (>4x avg=%luus) "
                      "calls=%lu max=%luus",
                      p->name, (ULONG)delta_us, (ULONG)avg_us,
                      (ULONG)p->calls, (ULONG)p->max_us);
            }
        }
    }
}

/* -----------------------------------------------------------------------
 * Per-format row converters.
 *
 * Each converter reads `w` source pixels and writes `w` X8R8G8B8 pixels
 * (the GPU resource format, see chip_virgl_2d.c header).  Signatures are
 * uniform so the outer loop can dispatch via a single function pointer.
 *
 * All three converters are safe on MEMF_SHARED / COHERENT / WRITETHROUGH
 * destination memory -- they use direct uint32 stores, no memcpy/memset.
 * ----------------------------------------------------------------------- */

/* 8bpp CLUT -> X8R8G8B8.  The palette table is pre-converted to the GPU
 * byte layout (X8R8G8B8 on PPC BE) in chip_SetColorArray, so a plain
 * indexed lookup produces the correct output pixels. */
static void conv_row_clut(const UBYTE *src, uint32 *dst, uint32 w,
                           const uint32 *palette)
{
    for (uint32 col = 0; col < w; col++)
        dst[col] = palette[src[col]];
}

/* 16bpp R5G6B5 -> B8G8R8X8.  Src uint16: [15:11]=R, [10:5]=G, [4:0]=B.
 * Channels expanded with LSB-fill so 0x1F -> 0xFF (instead of 0xF8).
 * Pack into B8G8R8X8: dst = (B<<24)|(G<<16)|(R<<8). */
static void conv_row_r5g6b5(const UBYTE *src, uint32 *dst, uint32 w,
                             const uint32 *palette)
{
    (void)palette;
    const uint16 *src16 = (const uint16 *)src;
    for (uint32 col = 0; col < w; col++) {
        uint16 v = src16[col];
        uint32 r = (v >> 11) & 0x1F;
        uint32 g = (v >>  5) & 0x3F;
        uint32 b =  v        & 0x1F;
        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);
        dst[col] = (b << 24) | (g << 16) | (r << 8);
    }
}

/* 32bpp A8R8G8B8 -> B8G8R8X8.  Src bytes [AA, RR, GG, BB] (uint32
 * 0xAARRGGBB on PPC BE) -> dst bytes [BB, GG, RR, AA] (uint32
 * 0xBBGGRRAA).  This is a 32-bit byte reverse, which the compiler
 * lowers to a single PPC `stwbrx` when the dst pointer is aligned.
 * Alpha byte naturally lands in the X position (irrelevant for X8R8G8B8
 * scanout, ignored by GL backend). */
static void conv_row_a8r8g8b8(const UBYTE *src, uint32 *dst, uint32 w,
                               const uint32 *palette)
{
    (void)palette;
    const uint32 *src32 = (const uint32 *)src;
    /* No early-out for src==dst here: format change means an in-place
     * byte-reverse is required even if buffers alias.  In practice
     * the DB-mode flush task always passes back_mem != panning_mem. */
    for (uint32 col = 0; col < w; col++) {
        uint32 s = src32[col];
        dst[col] = (s << 24)
                 | ((s & 0x0000FF00U) << 8)
                 | ((s & 0x00FF0000U) >> 8)
                 | (s >> 24);
    }
}

/* 32bpp B8G8R8A8 -> B8G8R8X8.  Src bytes [BB, GG, RR, AA] match the
 * destination layout exactly -- the alpha byte sits where X belongs.
 * Plain word copy. */
static void conv_row_b8g8r8a8(const UBYTE *src, uint32 *dst, uint32 w,
                                const uint32 *palette)
{
    (void)palette;
    const uint32 *src32 = (const uint32 *)src;
    if (src32 == dst) return;
    for (uint32 col = 0; col < w; col++)
        dst[col] = src32[col];
}

/* 32bpp R8G8B8A8 -> B8G8R8X8.  Src bytes [RR, GG, BB, AA].  Need
 * to swap byte 0 (R) with byte 2 (B); keep byte 1 (G) and byte 3 (A
 * = X). */
static void conv_row_r8g8b8a8(const UBYTE *src, uint32 *dst, uint32 w,
                                const uint32 *palette)
{
    (void)palette;
    const uint32 *src32 = (const uint32 *)src;
    for (uint32 col = 0; col < w; col++) {
        uint32 s = src32[col];
        dst[col] = (s & 0x00FF00FFU)               /* keep G at [23:16], A at [7:0] */
                 | ((s & 0x0000FF00U) << 16)       /* B (byte 2) -> top */
                 | ((s & 0xFF000000U) >> 16);      /* R (byte 0) -> bit [15:8] */
    }
}

/* 32bpp A8B8G8R8 -> B8G8R8X8.  Src bytes [AA, BB, GG, RR].  This is
 * a left rotation by 8 bits: source [A,B,G,R] becomes [B,G,R,A]. */
static void conv_row_a8b8g8r8(const UBYTE *src, uint32 *dst, uint32 w,
                                const uint32 *palette)
{
    (void)palette;
    const uint32 *src32 = (const uint32 *)src;
    for (uint32 col = 0; col < w; col++) {
        uint32 s = src32[col];
        dst[col] = (s << 8) | (s >> 24);   /* rlwinm rD,rS,8,0,31 */
    }
}

/* 16bpp R5G5B5 (1 unused MSB) -> B8G8R8X8.  format: 0rrrrrgggggbbbbb. */
static void conv_row_r5g5b5(const UBYTE *src, uint32 *dst, uint32 w,
                              const uint32 *palette)
{
    (void)palette;
    const uint16 *src16 = (const uint16 *)src;
    for (uint32 col = 0; col < w; col++) {
        uint16 v = src16[col];
        uint32 r = (v >> 10) & 0x1F;
        uint32 g = (v >>  5) & 0x1F;
        uint32 b =  v        & 0x1F;
        r = (r << 3) | (r >> 2);
        g = (g << 3) | (g >> 2);
        b = (b << 3) | (b >> 2);
        dst[col] = (b << 24) | (g << 16) | (r << 8);
    }
}

/* Signature for a row converter.  NULL when the format isn't supported. */
typedef void (*row_conv_fn)(const UBYTE *src, uint32 *dst, uint32 w,
                             const uint32 *palette);

/* Select the correct row converter for the active pixel format.
 * Returns NULL for unsupported formats so chip_convert_rect can bail. */
static row_conv_fn chip_pick_row_converter(RGBFTYPE fmt)
{
    switch (fmt) {
    case RGBFB_CLUT:      return conv_row_clut;
    case RGBFB_R5G6B5:    return conv_row_r5g6b5;
    case RGBFB_R5G5B5:    return conv_row_r5g5b5;
    case RGBFB_A8R8G8B8:  return conv_row_a8r8g8b8;
    case RGBFB_B8G8R8A8:  return conv_row_b8g8r8a8;
    case RGBFB_R8G8B8A8:  return conv_row_r8g8b8a8;
    case RGBFB_A8B8G8R8:  return conv_row_a8b8g8r8;
    default:              return NULL;
    }
}

/* -----------------------------------------------------------------------
 * chip_convert_rect -- convert panning_mem -> dst_mem for a dirty rect.
 *
 * Source reads from panning_mem (set by SetPanning) at active_bpr stride.
 * Dest is explicit (gs->fb_mem for single-buffer, back_mem for double-
 * buffer).  Format dispatch happens once before the row loop, via
 * chip_pick_row_converter -- adding a new depth only requires a new
 * conv_row_* function and a case in the selector.
 *
 * dst_mem is passed as an argument rather than using gs->fb_mem so that
 * concurrent flush_all calls (main task SetGC + periodic flush task) do
 * not race on a shared global pointer swap.
 * ----------------------------------------------------------------------- */
static void chip_convert_rect(struct ChipGPUState *gs, APTR dst_mem,
                               uint32 x, uint32 y, uint32 w, uint32 h)
{
    APTR src_base = gs->panning_mem ? gs->panning_mem : gs->board_mem;
    if (!src_base || !dst_mem) return;

    /* Profile per-frame board_mem -> fb conversion.  When the active
     * application uses GRANTDIRECTACCESS (graphics.library V54 writes
     * pixels directly to bi->MemoryBase, bypassing our vtable), this is
     * the only place that sees the work -- the flush task does a full-
     * frame conversion every refresh and its cost moves with the
     * application's write rate. */
    CHIP_PROF_DEFINE(prof_convert, "ConvertRect", 200);
    uint32 prof_t0 = chip_prof_begin(gs);

    row_conv_fn conv = chip_pick_row_converter(gs->active_format);
    if (!conv) {
        /* Unsupported format -- skip to avoid corrupting fb_mem.  Log once
         * per format so the cause of a black screen is obvious instead of
         * silent.  Two slots cover the realistic case of a CLUT->TrueColor
         * switch with a third unsupported format never seen on AOS4. */
        static uint32 warned_fmt[2] = { 0xFFFFFFFFU, 0xFFFFFFFFU };
        BOOL seen = FALSE;
        for (int i = 0; i < 2; i++)
            if (warned_fmt[i] == gs->active_format) { seen = TRUE; break; }
        if (!seen) {
            for (int i = 0; i < 2; i++) {
                if (warned_fmt[i] == 0xFFFFFFFFU) {
                    warned_fmt[i] = gs->active_format;
                    break;
                }
            }
            DCHIP("chip_convert_rect: UNSUPPORTED active_format=%lu -- "
                  "screen will stay black; add a converter to chip_flush.c",
                  (unsigned long)gs->active_format);
        }
        return;
    }

    uint32 src_bpp = chip_format_bpp(gs->active_format);
    uint32 pw = gs->panning_width ? gs->panning_width : gs->active_width;
    uint32 src_stride = pw * src_bpp;
    uint32 dst_stride = gs->fb_stride;

    /* Panning offset -- P96 uses xOff/yOff when virtual screen > display */
    uint32 pan_x = (gs->pan_xoff > 0) ? (uint32)gs->pan_xoff : 0;
    uint32 pan_y = (gs->pan_yoff > 0) ? (uint32)gs->pan_yoff : 0;

    /* Clamp to visible screen dimensions and GPU resource */
    uint32 max_w = gs->active_width < gs->fb_width ? gs->active_width : gs->fb_width;
    uint32 max_h = gs->active_height < gs->fb_height ? gs->active_height : gs->fb_height;
    if (x + w > max_w)  w = max_w - x;
    if (y + h > max_h)  h = max_h - y;
    if (w == 0 || h == 0) return;

    /* Clamp panning so we don't read past buffer end.  X is bounded by
     * panning_width.  Y has no explicit bitmap-height upper bound (the
     * bitmap can be virtual / larger than the active rect), so use the
     * GPU resource height as a hard ceiling -- panning past fb_height
     * would read past the source allocation entirely. */
    if (pan_x + max_w > pw) pan_x = pw > max_w ? pw - max_w : 0;
    if (pan_y + max_h > gs->fb_height) {
        static volatile uint32 pan_y_warned = 0;
        uint32 cur_y = pan_y;
        pan_y = gs->fb_height > max_h ? gs->fb_height - max_h : 0;
        if (!pan_y_warned) {
            pan_y_warned = 1;
            DCHIP("chip_convert_rect: pan_y=%lu would read past fb_height=%lu "
                  "(active=%lux%lu) -- clamped to %lu",
                  (ULONG)cur_y, (ULONG)gs->fb_height,
                  (ULONG)max_w, (ULONG)max_h, (ULONG)pan_y);
        }
    }

    for (uint32 row = y; row < y + h; row++) {
        const UBYTE *src_row = (const UBYTE *)src_base
                             + (row + pan_y) * src_stride
                             + (x + pan_x) * src_bpp;
        uint32 *dst_row = (uint32 *)((UBYTE *)dst_mem + row * dst_stride + x * 4);
        conv(src_row, dst_row, w, gs->palette);
    }

    /* Bytes processed = output pixel count * 4 (X8R8G8B8 destination). */
    chip_prof_end(gs, &prof_convert, prof_t0, w * h * 4);
}

/* -----------------------------------------------------------------------
 * chip_flush -- flush a dirty rectangle to the VirtIO GPU.
 * ----------------------------------------------------------------------- */
void chip_flush(WORD x, WORD y, UWORD w, UWORD h)
{
    struct ChipGPUState *gs = g_chip_state;
    if (!gs || !gs->fb_mem) return;

    uint32 max_w = gs->active_width  < gs->fb_width  ? gs->active_width  : gs->fb_width;
    uint32 max_h = gs->active_height < gs->fb_height ? gs->active_height : gs->fb_height;

    /* Clip to [0, max) in signed 32-bit space.  The previous UWORD-based
     * clamp wrapped when a rect lay entirely off-screen left/top
     * (w += negative x underflowed to ~65k and got re-clamped to a
     * full-width flush instead of being skipped). */
    LONG x0 = x, y0 = y;
    LONG x1 = (LONG)x + (LONG)w;
    LONG y1 = (LONG)y + (LONG)h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (LONG)max_w) x1 = (LONG)max_w;
    if (y1 > (LONG)max_h) y1 = (LONG)max_h;
    if (x0 >= x1 || y0 >= y1)
        return;
    x = (WORD)x0;          y = (WORD)y0;
    w = (UWORD)(x1 - x0);  h = (UWORD)(y1 - y0);

    /* Wake the flush task and return -- NO synchronous presentation.
     *
     * This used to convert + TRANSFER_TO_HOST + RESOURCE_FLUSH the rect
     * inline (two synchronous VirtIO round-trips under io_lock) on the
     * single-buffer path.  That made every FillRect / BlitTemplate /
     * DrawLine pay a fixed ~2.5 ms regardless of size: gfxbench2d
     * measured FillRect at ~400 ops/s flat from 16x16 to 512x512 while
     * BltBitMap (whose path was already signal-only) ran at 254,000
     * ops/s.  The synchronous work was also redundant: the flush task
     * presents a full frame on every wake anyway (it has to -- with
     * BIF_GRANTDIRECTACCESS the OS writes pixels behind the vtable's
     * back, so only a full-frame pass catches everything).
     *
     * Latency is unchanged: the signal wakes the higher-priority flush
     * task immediately, exactly as it always did for BlitRect/NMC. */
    chip_flush_signal_activity(gs);
}

/* -----------------------------------------------------------------------
 * chip_flush_all -- flush the entire visible area as a single batched op.
 *
 * Double-buffer mode (preferred):
 *   Phase 1: Convert board_mem -> back fb_mem (whole frame, no lock).
 *   Phase 2: Single TransferToHost + SET_SCANOUT + RESOURCE_FLUSH (locked).
 *   Phase 3: Swap primary <-> *2 fields so primary always = displayed (front).
 *   While GPU displays front, next frame writes to back -- no tearing.
 *
 * Single-buffer fallback:
 *   Convert + transfer + flush in one locked sequence.
 * ----------------------------------------------------------------------- */

/* Helper: swap front <-> back buffer state under io_lock */
static void chip_swap_buffers(struct ChipGPUState *gs)
{
    /* Swap fb_mem pointers */
    APTR tmp_mem = gs->fb_mem;
    gs->fb_mem = gs->fb_mem2;
    gs->fb_mem2 = tmp_mem;

    uint32 tmp32 = gs->fb_phys;
    gs->fb_phys = gs->fb_phys2;
    gs->fb_phys2 = tmp32;

    tmp32 = gs->fb_dma_count;
    gs->fb_dma_count = gs->fb_dma_count2;
    gs->fb_dma_count2 = tmp32;

    struct DMAEntry *tmp_dl = gs->fb_dma_list;
    gs->fb_dma_list = gs->fb_dma_list2;
    gs->fb_dma_list2 = tmp_dl;

    tmp32 = gs->fb_resource_id;
    gs->fb_resource_id = gs->fb_resource_id2;
    gs->fb_resource_id2 = tmp32;

    tmp32 = gs->virgl_2d_resource;
    gs->virgl_2d_resource = gs->virgl_2d_resource2;
    gs->virgl_2d_resource2 = tmp32;

    /* Update resource_id to match new front */
    gs->resource_id = (gs->virgl_2d_ready && !gs->virgl_ctx_error && gs->virgl_2d_resource)
                      ? gs->virgl_2d_resource : gs->fb_resource_id;
}

void chip_flush_all(void)
{
    struct ChipGPUState *gs = g_chip_state;
    if (!gs || !gs->fb_mem) return;

    uint32 w = gs->active_width  < gs->fb_width  ? gs->active_width  : gs->fb_width;
    uint32 h = gs->active_height < gs->fb_height ? gs->active_height : gs->fb_height;
    if (w == 0 || h == 0) return;

    struct ExecIFace *IExec = gs->IExec;

    /* Profile the full per-frame flush cycle (convert + transfer +
     * scanout + flush + buffer swap).  Together with PROF[ConvertRect]
     * we can attribute time between CPU work (convert) and GPU
     * round-trips (everything else). */
    CHIP_PROF_DEFINE(prof_flush_all, "FlushAll", 200);
    uint32 prof_t0 = chip_prof_begin(gs);

    HOT_STAGE(gs, "flush_all:start");

    if (!gs->double_buffer || !gs->fb_mem2) {
        /* --- Single-buffer path: batched single-transfer flush.
         * Converts whole frame, then one TransferToHost covering entire area,
         * then one ResourceFlush.  io_lock must be held for the conversion
         * because chip_flush (direct drawing op) can race with this path in
         * single-buffer mode. */
        HOT_STAGE(gs, "flush_all:sb:obtain_lock");
        IExec->MutexObtain(gs->io_lock);
        HOT_STAGE(gs, "flush_all:sb:convert");

        chip_convert_rect(gs, gs->fb_mem, 0, 0, w, h);

        BOOL t_ok;
        if (gs->virgl_2d_ready && !gs->virgl_ctx_error) {
            struct virtio_gpu_box box;
            chip_zero(&box, sizeof(box));
            box.w = w; box.h = h; box.d = 1;
            t_ok = chip_TransferToHost3D(gs, gs->virgl_2d_ctx, gs->resource_id,
                0, gs->fb_stride, 0, 0, &box);
            if (!t_ok) {
                gs->virgl_ctx_error = TRUE;
                t_ok = chip_TransferToHost2D(gs, gs->fb_resource_id, 0, 0, w, h, 0ULL);
            }
        } else {
            t_ok = chip_TransferToHost2D(gs, gs->fb_resource_id, 0, 0, w, h, 0ULL);
        }

        if (t_ok) {
            HOT_STAGE(gs, "flush_all:sb:res_flush");
            uint32 flush_res = (gs->virgl_ctx_error || !gs->virgl_2d_ready)
                                ? gs->fb_resource_id : gs->resource_id;
            chip_ResourceFlush(gs, flush_res, 0, 0, w, h);
        }

        HOT_STAGE(gs, "flush_all:sb:release_lock");
        IExec->MutexRelease(gs->io_lock);
        HOT_STAGE(gs, "flush_all:sb:done");
        chip_prof_end(gs, &prof_flush_all, prof_t0, w * h * 4);
        return;
    }

    /* --- Double-buffer path: write to back, then swap --- */
    static uint32 db_swap_count = 0;

#ifdef CHIP_DEBUG_VERBOSE
    /* Diagnostic (verbose builds only): sample a pixel from the source to
     * confirm panning_mem / format / stride are consistent.  Fires only
     * once per session at 640x480 -- the common boot resolution where
     * CLUT/format-conversion bugs show up first. */
    static uint32 flush_diag_count = 0;
    if (h == 480 && flush_diag_count < 3) {
        APTR src = gs->panning_mem ? gs->panning_mem : gs->board_mem;
        if (src) {
            uint32 src_bpp = chip_format_bpp(gs->active_format);
            uint32 pw = gs->panning_width ? gs->panning_width : w;
            uint32 src_stride = pw * src_bpp;
            uint32 *px = (uint32 *)((UBYTE *)src + 100 * src_stride + 100 * src_bpp);
            IExec->DebugPrintF("[virtiogpu.chip] flush_all diag: %lux%lu panning=%p fmt=%ld "
                               "pw=%lu bpp=%lu px@(100,100)=0x%08lx\n",
                               (unsigned long)w, (unsigned long)h, src,
                               (LONG)gs->active_format, (unsigned long)pw,
                               (unsigned long)src_bpp, (unsigned long)*px);
        }
        flush_diag_count++;
    }
#endif

    /* Snapshot back-buffer selection OUTSIDE the io_lock so the
     * CPU-heavy 4MB chip_convert_rect() runs without holding the GPU
     * serialisation lock.  Only the flush task ever writes to back_mem
     * (chip_flush_all has a single caller in double-buffer mode), and
     * back_* fields are only mutated inside io_lock by chip_swap_buffers
     * at the end of this function -- the snapshot values below are
     * stable for the duration of this iteration.
     *
     * The big win: during gfxbench2d's Overlapped/Random phases the
     * convert takes ~30ms and the GPU commands take many seconds of
     * waiting.  Previously io_lock was held for the whole duration,
     * blocking any other vtable op that needs the lock (SetScanout,
     * cursor updates via the comp hook, etc.).  With convert outside
     * the lock, io_lock only covers the actual GPU round-trips. */
    APTR    back_mem    = gs->fb_mem2;
    uint32  back_2d_res = gs->fb_resource_id2;
    uint32  back_3d_res = gs->virgl_2d_resource2;
    BOOL    use_virgl   = gs->virgl_2d_ready && !gs->virgl_ctx_error && back_3d_res;

    HOT_STAGE(gs, "flush_all:db:convert");
    chip_convert_rect(gs, back_mem, 0, 0, w, h);
    HOT_STAGE(gs, "flush_all:db:obtain_lock");
    IExec->MutexObtain(gs->io_lock);
    HOT_STAGE(gs, "flush_all:db:transfer");

    BOOL t_ok;
    if (use_virgl) {
        struct virtio_gpu_box box;
        chip_zero(&box, sizeof(box));
        box.w = w; box.h = h; box.d = 1;
        t_ok = chip_TransferToHost3D(gs, gs->virgl_2d_ctx, back_3d_res,
            0, gs->fb_stride, 0, 0, &box);
        if (!t_ok) {
            DCHIP("flush_all(db): TransferToHost3D FAILED -- context poisoned");
            gs->virgl_ctx_error = TRUE;
            use_virgl = FALSE;
            t_ok = chip_TransferToHost2D(gs, back_2d_res, 0, 0, w, h, 0ULL);
        }
    } else {
        t_ok = chip_TransferToHost2D(gs, back_2d_res, 0, 0, w, h, 0ULL);
    }

    if (!t_ok) {
        IExec->MutexRelease(gs->io_lock);
        return;
    }

    uint32 back_res = use_virgl ? back_3d_res : back_2d_res;
    HOT_STAGE(gs, "flush_all:db:scanout");
    chip_SetScanout(gs, 0, back_res, 0, 0, w, h);
    HOT_STAGE(gs, "flush_all:db:res_flush");
    chip_ResourceFlush(gs, back_res, 0, 0, w, h);
    HOT_STAGE(gs, "flush_all:db:swap");

    /* Phase 3: Swap front <-> back pointers.
     * After this, primary fields (fb_mem, resource_id, etc.) = new front. */
    chip_swap_buffers(gs);

    db_swap_count++;
    /* Swap logging: only the first ~10 messages (every 200 swaps, for the
     * first 2000 swaps).  Long-running sessions generate thousands of swaps
     * so unbounded logging quickly drowns useful diagnostics.  The initial
     * burst is enough to confirm double-buffering started correctly; after
     * that, silence until something actually changes. */
    if (db_swap_count <= 2000 && (db_swap_count % 200) < 2) {
        DCHIP("flush_all(db): swap #%lu front_res=%lu back_res=%lu virgl=%s %lux%lu",
              (unsigned long)db_swap_count,
              (unsigned long)gs->resource_id,
              (unsigned long)(use_virgl ? gs->virgl_2d_resource2 : gs->fb_resource_id2),
              use_virgl ? "3D" : "2D",
              (unsigned long)w, (unsigned long)h);
    }

    HOT_STAGE(gs, "flush_all:db:release_lock");
    IExec->MutexRelease(gs->io_lock);
    HOT_STAGE(gs, "flush_all:db:done");
    chip_prof_end(gs, &prof_flush_all, prof_t0, w * h * 4);
}

/* -----------------------------------------------------------------------
 * Periodic flush task -- runs in background, flushes board_mem -> GPU
 * every ~20ms so that direct P96 writes (cursor, window content via
 * BIF_GRANTDIRECTACCESS) become visible without explicit chip callbacks.
 * ----------------------------------------------------------------------- */
/* -----------------------------------------------------------------------
 * chip_flush_signal_activity -- wake the flush task immediately.
 *
 * Called from vtable drawing callbacks and screen-switch callbacks
 * (SetPanning / SetDisplay / SetSwitch) instead of invoking
 * chip_flush_all() directly.  The signal wakes the flush task out of
 * its Wait(), cancels the idle-backstop timer, and lets it do a single
 * coalesced flush.  No blocking for the caller.
 *
 * NULL-safe: no-op if the flush task hasn't been created yet (some
 * callbacks run before SetDisplay(1)/SetSwitch(1) have spawned it).
 * ----------------------------------------------------------------------- */
void chip_flush_signal_activity(struct ChipGPUState *gs)
{
    if (!gs || !gs->flush_task || !gs->flush_sig_mask) return;
    gs->stat_signal_calls++;
    gs->IExec->Signal(gs->flush_task, gs->flush_sig_mask);
}

/* -----------------------------------------------------------------------
 * chip_flush_task_entry -- signal-driven flush task with vblank backstop.
 *
 * Loop structure:
 *   1. Arm a timer.device request for gs->refresh_us (the active mode's
 *      refresh interval, recomputed in chip_SetGC).
 *   2. Wait() on the activity signal, the timer port signal, or CTRL_C.
 *   3. If activity fired: abort the pending timer (clean cancellation)
 *      so we don't consume a spurious timer message on the next loop.
 *      If timer fired: GetMsg to drain the reply.
 *   4. Flush board_mem to the GPU.
 *   5. Loop.  If activity is continuous, SetSignal clears the bit on
 *      entry to Wait, and any subsequent Signal() re-wakes us instantly
 *      -- no cap on flush rate (only bounded by VirtIO round-trip time).
 *
 * Idle behaviour: no signals -> Wait sleeps for refresh_us -> timer fires
 * -> one flush per refresh (catches GRANTDIRECTACCESS writes and cursor
 * updates without wasting bandwidth on unchanged frames).
 * ----------------------------------------------------------------------- */
void chip_flush_task_entry(void)
{
    struct ChipGPUState *gs = g_chip_state;
    if (!gs) return;
    struct ExecIFace *IExec = gs->IExec;

    /* Reply port for timer.device completion messages.  One signal bit
     * allocated by AllocSysObject -- used only for the vblank backstop. */
    struct MsgPort *port = IExec->AllocSysObjectTags(ASOT_PORT, TAG_DONE);
    if (!port) { DCHIP("flush_task: AllocPort failed"); return; }

    struct TimeRequest *tr = (struct TimeRequest *)IExec->AllocSysObjectTags(
        ASOT_IOREQUEST,
        ASOIOR_Size, sizeof(struct TimeRequest),
        ASOIOR_ReplyPort, (ULONG)port,
        TAG_DONE);
    if (!tr) {
        DCHIP("flush_task: AllocIORequest failed");
        IExec->FreeSysObject(ASOT_PORT, port);
        return;
    }

    if (IExec->OpenDevice("timer.device", UNIT_MICROHZ,
                           (struct IORequest *)tr, 0) != 0) {
        DCHIP("flush_task: OpenDevice timer.device failed");
        IExec->FreeSysObject(ASOT_IOREQUEST, tr);
        IExec->FreeSysObject(ASOT_PORT, port);
        return;
    }

    /* Acquire ITimer for ReadEClock -- used by chip_GetCurrentY to compute
     * the synthesised fractional-frame Y position from EClock delta.
     * Non-fatal: if it fails, GetCurrentY just returns 0. */
    struct TimerIFace *ITimer = (struct TimerIFace *)IExec->GetInterface(
        (struct Library *)tr->Request.io_Device, "main", 1, NULL);
    if (!ITimer) {
        DCHIP("flush_task: WARNING -- ITimer GetInterface failed, "
              "GetCurrentY will return 0");
    }

    /* Activity signal -- allocated here so only the flush task owns it.
     * The signal bit + task pointer are stored in gs for other tasks
     * to Signal() us via chip_flush_signal_activity. */
    int8 sig_bit = IExec->AllocSignal(-1);
    if (sig_bit < 0) {
        DCHIP("flush_task: AllocSignal failed");
        if (ITimer) IExec->DropInterface((struct Interface *)ITimer);
        IExec->CloseDevice((struct IORequest *)tr);
        IExec->FreeSysObject(ASOT_IOREQUEST, tr);
        IExec->FreeSysObject(ASOT_PORT, port);
        return;
    }
    uint32 flush_sig = 1UL << sig_bit;
    uint32 timer_sig = 1UL << port->mp_SigBit;
    uint32 wait_mask = flush_sig | timer_sig | SIGBREAKF_CTRL_C;

    gs->flush_task     = IExec->FindTask(NULL);
    gs->flush_sig_bit  = sig_bit;
    gs->flush_sig_mask = flush_sig;

    /* Default idle interval: 60 Hz if SetGC hasn't populated refresh_us yet. */
    if (gs->refresh_us == 0) gs->refresh_us = 16667;

    DCHIP("flush_task: started (signal-driven, idle=%luus) virgl_2d=%s res=%lu itimer=%s",
          (ULONG)gs->refresh_us,
          gs->virgl_2d_ready ? "YES" : "NO", gs->resource_id,
          ITimer ? "ON" : "OFF");

    BOOL timer_pending = FALSE;
    uint32 cycle = 0;

    /* Publish ITimer + EClock frequency so chip_GetCurrentY (running in
     * other tasks) can compute fractional-frame Y position via
     * ITimer->ReadEClock without opening its own timer.device. */
    ULONG eclock_freq = 0;
    if (ITimer) {
        struct EClockVal initial_ev;
        eclock_freq = ITimer->ReadEClock(&initial_ev);
        gs->last_vsync_eclock_lo = initial_ev.ev_lo;
    }
    gs->ITimer      = ITimer;
    gs->eclock_freq = (uint32)eclock_freq;

    HOT_STAGE(gs, "loop:enter");

    /* Normal flush task: wake on either the idle-backstop timer or an
     * activity signal from a drawing callback.  The timer acts as a
     * vblank backstop for GRANTDIRECTACCESS writes; the signal gives
     * prompt refresh after vtable drawing ops.  Task priority is 10
     * (see chip_SetDisplay), which ensures we preempt CPU-heavy user
     * tasks (e.g. gfxbench2d) reliably -- without that, intensive
     * drawing loops at pri 0 starved this task at pri 0 and the
     * screen stalled. */

    while (!gs->flush_task_quit) {
        gs->hot_iter++;
        HOT_STAGE(gs, "loop:top");

        /* Arm the pacing timer. */
        if (!timer_pending) {
            HOT_STAGE(gs, "timer:arming");
            tr->Request.io_Command = TR_ADDREQUEST;
            tr->Time.Seconds       = 0;
            tr->Time.Microseconds  = gs->refresh_us;
            IExec->SendIO((struct IORequest *)tr);
            gs->hot_timers_armed++;
            timer_pending = TRUE;
            HOT_STAGE(gs, "timer:armed");
        }

        HOT_STAGE(gs, "wait:enter");
        uint32 signals = IExec->Wait(wait_mask);
        HOT_STAGE(gs, "wait:return");

        if (signals & SIGBREAKF_CTRL_C) break;
        if (gs->flush_task_quit) break;

        BOOL was_timer  = (signals & timer_sig) != 0;
        BOOL was_signal = (signals & flush_sig) != 0;
        if (was_timer) {
            HOT_STAGE(gs, "timer:getmsg");
            IExec->GetMsg(port);
            gs->hot_timers_fired++;
            timer_pending = FALSE;
        }
        if (was_signal) {
            HOT_STAGE(gs, "flush_sig:clear");
            IExec->SetSignal(0, flush_sig);
        }

        /* Skip flush when display is powered off (DPMS).
         *
         * Originally also gated on `gs->rtg_output_enabled` (set by
         * chip_SetSwitch), but that's wrong for AOS4 -- canonical
         * Picasso96 SetSwitch is for physical VGA pass-through cards
         * toggling the display chain between native chipset and RTG.
         * AOS4 QEMU Pegasos2 has no native chipset to switch *from*,
         * so graphics.library calls SetSwitch(FALSE) once at boot to
         * indicate "RTG output not currently routed" and never calls
         * SetSwitch(TRUE) again.  RTG content is driven via SetDisplay
         * + the normal flush path.  Gating on rtg_output_enabled
         * therefore left every flush task wake-up after boot skipping
         * the host transfer (black screen).  Field kept in
         * ChipGPUState as a stat counter; just don't use it to suppress
         * work. */
        if (gs->dpms_level != 0) {
            HOT_STAGE(gs, "dpms:skip");
            cycle++;
            continue;
        }

        /* Flip the synthetic vblank phase on every wakeup.  The VirtIO
         * GPU has no real vblank signal, so we synthesise one off the
         * flush cadence -- apps polling chip_GetVSyncState see a level
         * change at least once per refresh interval, which is what they
         * use to detect frame boundaries. */
        gs->vsync_phase ^= 1;

        /* Stamp the vsync time so chip_GetCurrentY can derive fractional
         * frame position from EClock delta.  Cheap (register read). */
        if (ITimer) {
            struct EClockVal ev;
            ITimer->ReadEClock(&ev);
            gs->last_vsync_eclock_lo = ev.ev_lo;
        }

        /* Wake any task blocked in chip_WaitVerticalSync.  Single-slot
         * rendezvous: take the registration under vsync_mutex, clear
         * it, then Signal outside the critical section.  If no waiter,
         * this is two volatile reads -- the mutex is only acquired
         * when there's actually something to do.  AOS4 mutex preferred
         * over Forbid/Permit so the rest of the system isn't frozen
         * during the signal-out path. */
        if (gs->wait_vsync_task && gs->vsync_mutex) {
            struct Task *vt = NULL;
            uint32       vm = 0;
            IExec->MutexObtain(gs->vsync_mutex);
            vt = (struct Task *)gs->wait_vsync_task;
            vm = gs->wait_vsync_mask;
            gs->wait_vsync_task = NULL;
            gs->wait_vsync_mask = 0;
            IExec->MutexRelease(gs->vsync_mutex);
            if (vt && vm) IExec->Signal(vt, vm);
        }

        /* If BIF_VBLANKINTERRUPT is advertised AND the chip's
         * SetInterrupt has been enabled, fire bi->SoftInterrupt now.
         * rtg.library / graphics.library installs its own SoftInterrupt
         * handler that walks bi->WaitQ and signals every task waiting
         * on WaitTOF / WaitBOVP -- we don't need to know the WaitQ node
         * format because rtg.library handles the queue internally.
         *
         * Pattern verified against canonical PiccoloSD64.card.asm
         * (HardInterrupt -> Cause(SoftInterrupt)) and confirmed
         * implementable on VirtIO GPU: we have no real hardware vblank
         * IRQ, but Cause() is callable from any task context and the
         * flush task already wakes once per refresh interval, which
         * gives apps a stable WaitTOF cadence. */
        if (gs->vbi_enabled && gs->bi
            && gs->bi->SoftInterrupt.is_Code != NULL) {
            /* Belt-and-braces: only Cause() once rtg.library has
             * actually installed its WaitQ-walker handler.  We set
             * BIF_VBLANKINTERRUPT in chip_InitCard and SetInterrupt
             * latches vbi_enabled, but neither guarantees the
             * SoftInterrupt's is_Code/is_Data are populated.  A
             * Cause() on a zero-is_Code Interrupt would bus-error. */
            IExec->Cause(&gs->bi->SoftInterrupt);
        }

        /* Recover from poisoned Virgl context before flushing. */
        if (gs->virgl_ctx_error) {
            chip_virgl_recover_2d(gs);
        }

        /* Main work -- always full-frame because GRANTDIRECTACCESS means
         * we cannot know exactly which pixels changed. */
        HOT_STAGE(gs, "flush_all:enter");
        gs->hot_flush_entries++;
        chip_flush_all();
        gs->hot_flush_exits++;
        HOT_STAGE(gs, "flush_all:exit");

        /* Optional: persistent debug overlay after each transfer. */
        if (gs->virgl_test_quad == 1) {
            chip_virgl_draw_colored_quad(gs,
                200, 100, 160, 120,
                1.0f, 0.0f, 0.0f, 0.8f);
        } else if (gs->virgl_test_quad == 2) {
            chip_comp_test_textured_quad(gs);
        }

        cycle++;

        /* Poll for VirtIO GPU config-change events every ~50 cycles.
         * With active-mode flushing this can be sub-second, with idle-mode
         * this is roughly once per second -- acceptable granularity for
         * monitor hot-plug / resolution-change notifications. */
        if ((cycle % 50) == 0 && gs->device_cfg_base) {
            uint32 events = DEVCFG_R32(gs, 0);
            if (events & 1) {                  /* VIRTIO_GPU_EVENT_DISPLAY */
                DEVCFG_W32(gs, 4, 1);          /* events_clear */
                chip_handle_display_change(gs);
            }
        }
    }

    /* Clean shutdown: drain any outstanding timer, release the signal
     * bit, null out the published task pointer so late Signals are
     * harmless, then close the device and free allocations.
     *
     * We do NOT call AbortIO here -- see the note in the main loop.
     * The shutdown path runs at driver unload which is rare, so the
     * worst case (blocking up to refresh_us) is acceptable. */
    if (timer_pending) {
        IExec->WaitIO((struct IORequest *)tr);
    }
    gs->flush_sig_mask = 0;
    gs->flush_task     = NULL;
    if (gs->flush_sig_bit >= 0) {
        IExec->FreeSignal(gs->flush_sig_bit);
        gs->flush_sig_bit = -1;
    }

    /* Retract published ITimer pointer before dropping the interface
     * so chip_GetCurrentY (running in other tasks) won't dereference
     * a stale pointer.  Compiler barrier keeps the order. */
    gs->ITimer = NULL;
    __asm__ volatile ("" ::: "memory");
    if (ITimer) IExec->DropInterface((struct Interface *)ITimer);
    IExec->CloseDevice((struct IORequest *)tr);
    IExec->FreeSysObject(ASOT_IOREQUEST, tr);
    IExec->FreeSysObject(ASOT_PORT, port);
    DCHIP("flush_task: exiting (iter=%lu flush_all=%lu/%lu wait=%lu/%lu)",
          (ULONG)gs->hot_iter,
          (ULONG)gs->hot_flush_entries, (ULONG)gs->hot_flush_exits,
          (ULONG)gs->hot_wait_entries, (ULONG)gs->hot_wait_exits);
}
