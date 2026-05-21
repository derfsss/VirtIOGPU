/*
 * chip_p96.c -- Picasso96 BoardInfo vtable functions.
 *
 * All P96 callback functions: display config (SetSwitch, SetGC, SetPanning,
 * SetColorArray, SetDAC, SetDisplay, SetDPMSLevel), pixel clock (Resolve,
 * GetPixelClock), drawing ops (FillRect, BlitRect, InvertRect, etc.),
 * memory allocation (AllocBitMap, AllocCardMem), and software cursor.
 *
 * chip_fill_boardinfo_vtable() is the single exported function -- it assigns
 * all function pointers into the BoardInfo struct.
 */

#include "chip/chip_state.h"
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/dos.h>

/* -----------------------------------------------------------------------
 * Stubs for unimplemented vtable entries.
 *
 * Each stub logs once on first invocation so we can see in the boot log
 * whether AOS4 actually exercises the slot.  After the first hit the
 * stub falls silent (a 32-bit bitmask covers up to 32 distinct stubs).
 * If a stub turns out to be on a hot path, that's the signal to
 * implement it properly.
 * ----------------------------------------------------------------------- */
static volatile uint32 stub_seen_mask = 0;

static inline void stub_log_once(uint32 bit, const char *name)
{
    uint32 m = 1U << bit;
    if (!(stub_seen_mask & m)) {
        stub_seen_mask |= m;
        DCHIP("STUB: %s called (BoardInfo vtable slot bit=%lu) -- "
              "AOS4 invoked an unimplemented entry; no-op return",
              name, (unsigned long)bit);
    }
}

#define STUB_VOID(NAME, BIT)                                                  \
    static void stub_##NAME(void) { stub_log_once((BIT), #NAME); }

STUB_VOID(SetClock,      0)
STUB_VOID(SetMemoryMode, 1)
STUB_VOID(SetWriteMask,  2)
STUB_VOID(SetClearMask,  3)
STUB_VOID(SetReadPlane,  4)
STUB_VOID(WaitBlitter,   5)
STUB_VOID(ResetChip,     6)
STUB_VOID(fp21,          7)
STUB_VOID(fp22,          8)
STUB_VOID(fp23,          9)
STUB_VOID(fp24,         10)
STUB_VOID(fp43,         11)
STUB_VOID(fp44,         12)
STUB_VOID(fp45,         13)
STUB_VOID(fp46,         14)
STUB_VOID(fp47,         15)
STUB_VOID(fp48,         16)
STUB_VOID(fp49,         17)
STUB_VOID(fp50,         18)
STUB_VOID(fp51,         19)
STUB_VOID(fp52,         20)
STUB_VOID(fp57,         21)
STUB_VOID(fp65,         22)
STUB_VOID(fp66,         23)
STUB_VOID(fp67,         24)

/* The bitmap/board memory allocator and all blitting/drawing
 * primitives have been split out:
 *
 *   chip_alloc.c -- board_alloc/_free, chip_AllocBitMap, chip_FreeBitMap,
 *                   chip_AllocCardMem, chip_FreeCardMem, chip_GetBitMapAttr.
 *                   Wired via chip_alloc_fill_vtable() (proto in chip_state.h).
 *
 *   chip_blit.c  -- chip_FillRect, chip_InvertRect, chip_BlitRect,
 *                   chip_BlitRectNoMaskComplete, chip_BlitTemplate,
 *                   chip_BlitPattern, chip_DrawLine, chip_BlitPlanar2Chunky,
 *                   chip_BlitPlanar2Direct.
 *                   Wired via chip_blit_fill_vtable().
 *
 * What stays in this file: display configuration (SetSwitch, SetGC,
 * SetPanning, SetDAC, SetColorArray, SetDisplay, SetDPMSLevel, vsync),
 * pixel clock (Resolve, GetPixelClock), and the sprite callbacks. */


/* -----------------------------------------------------------------------
 * Display configuration vtable functions
 * ----------------------------------------------------------------------- */
/* -----------------------------------------------------------------------
 * chip_apply_null_vtable_overrides -- joerg-on-amigans.net experiment.
 *
 * "nearly all BoardInfo functions in a P96 driver can be set to NULL on
 * AmigaOS 4.x and a fallback function in graphics.library is used
 * instead" -- if the library fallback is faster than the chip's CPU
 * implementation (especially for the slow CLUT8 test-screen FillRect
 * burst we measured), this lets us A/B compare without a rebuild.
 *
 * Toggle at runtime by creating ENV:virtiogpu_null_vtable
 *   `Echo "" >ENV:virtiogpu_null_vtable`   (enable, then reboot QEMU)
 *   `Unset virtiogpu_null_vtable`          (disable, reboot)
 * The check is deferred to chip_SetSwitch(TRUE) because dos.library and
 * ENV: aren't ready during chip_InitCard_C (which runs before
 * Startup-Sequence).
 *
 * Only the optional drawing-op slots are nulled.  Required slots
 * (memory mgmt, display config, sprite, palette) stay wired.
 * ----------------------------------------------------------------------- */
static void chip_apply_null_vtable_overrides(struct ChipGPUState *gs,
                                              struct BoardInfo *bi)
{
    struct ExecIFace *IExec = gs->IExec;
    if (!gs->DOSBase) {
        gs->DOSBase = IExec->OpenLibrary("dos.library", 54);
        if (gs->DOSBase && !gs->IDOS)
            gs->IDOS = (struct DOSIFace *)
                IExec->GetInterface(gs->DOSBase, "main", 1, NULL);
    }
    if (!gs->IDOS) {
        DCHIP("null-vtable check: dos.library unavailable -- skipping");
        return;
    }

    char buf[8];
    LONG got = gs->IDOS->GetVar("virtiogpu_null_vtable",
                                 buf, sizeof(buf), 0);
    if (got < 0) {
        DCHIP("null-vtable mode: DISABLED (ENV:virtiogpu_null_vtable not set)");
        return;
    }

    DCHIP("null-vtable mode: ENABLED -- NULLing optional vtable slots "
          "(set ENV:virtiogpu_null_vtable=%s)", buf);

    /* Nine drawing operations + their FB twins = 18 slots.  Each pair is
     * the canonical entry and the ForceFB variant (called when AOS4
     * wants the operation against the live framebuffer rather than an
     * offscreen RenderInfo). */
    bi->FillRect                 = NULL;
    bi->FillRectFB               = NULL;
    bi->InvertRect               = NULL;
    bi->InvertRectFB             = NULL;
    bi->BlitRect                 = NULL;
    bi->BlitRectFB               = NULL;
    bi->BlitTemplate             = NULL;
    bi->BlitTemplateFB           = NULL;
    bi->BlitPattern              = NULL;
    bi->BlitPatternFB            = NULL;
    bi->DrawLine                 = NULL;
    bi->DrawLineFB               = NULL;
    bi->BlitRectNoMaskComplete   = NULL;
    bi->BlitRectNoMaskCompleteFB = NULL;
    bi->BlitPlanar2Chunky        = NULL;
    bi->BlitPlanar2ChunkyFB      = NULL;
    bi->BlitPlanar2Direct        = NULL;
    bi->BlitPlanar2DirectFB      = NULL;

    DCHIP("null-vtable mode: 18 drawing-op slots cleared -- "
          "graphics.library fallbacks will handle FillRect/BlitRect/etc.");
}

static BOOL chip_SetSwitch(struct BoardInfo *bi, BOOL enabled)
{
    (void)bi;
    DCHIP("SetSwitch(%ld) MemBase=%p", (LONG)enabled, bi ? bi->MemoryBase : NULL);

    struct ChipGPUState *gs = g_chip_state;
    if (gs) {
        gs->stat_setswitch++;
        /* Latch the requested state so the flush task can suppress
         * transfers when RTG output is parked.  Canonical contract:
         * SetSwitch(FALSE) hands the display chain back to native
         * Amiga or another board; the chip should stop driving its
         * scanout until SetSwitch(TRUE) is called again. */
        gs->rtg_output_enabled = enabled ? TRUE : FALSE;
    }

    /* Fallback flush creation via dos.library -- only if exec AddTask in
     * SetDisplay didn't already start one. */
    if (enabled && gs && !gs->flush_running) {
        if (!gs->DOSBase) {
            gs->DOSBase = gs->IExec->OpenLibrary("dos.library", 54);
            if (gs->DOSBase)
                gs->IDOS = (struct DOSIFace *)
                    gs->IExec->GetInterface(gs->DOSBase, "main", 1, NULL);
        }
        if (gs->IDOS) {
            gs->flush_proc = gs->IDOS->CreateNewProcTags(
                NP_Entry,     (ULONG)chip_flush_task_entry,
                NP_Name,      (ULONG)"virtiogpu.flush",
                NP_StackSize, 32768,
                NP_Priority,  10,  /* match the AddTask primary path */
                NP_Child,     TRUE,
                TAG_DONE);
            if (gs->flush_proc) {
                gs->flush_running = TRUE;
                DCHIP("Flush process created (dos fallback): %p", gs->flush_proc);
            } else {
                DCHIP("WARNING: dos flush process creation failed");
            }
        } else {
            DCHIP("WARNING: dos.library still not available for flush task");
        }
    }

    /* Set DIPF_IS_HWCOMPOSITE on our display modes (once, deferred to here
     * because display database isn't ready during chip_InitCard_C) */
    if (enabled && gs && gs->virgl_2d_ready) {
        chip_comp_set_dipf_flags(gs);
    }

    /* One-shot DisplayInfoBase dump after the database is populated --
     * lets us see exactly which ModeIDs ScreenMode prefs will offer and
     * who registered them. */
    if (enabled && gs && !gs->displayinfo_dumped) {
        chip_dump_displayinfobase(gs);
        gs->displayinfo_dumped = TRUE;
    }

    /* One-shot null-vtable override check.  Defers to first SetSwitch
     * because dos.library / ENV: aren't ready during chip_InitCard_C.
     * See chip_apply_null_vtable_overrides comment for usage. */
    if (enabled && gs && !gs->null_vtable_checked) {
        chip_apply_null_vtable_overrides(gs, bi);
        gs->null_vtable_checked = TRUE;
    }

    if (enabled) chip_flush_signal_activity(gs);
    return TRUE;
}

static void chip_SetColorArray(struct BoardInfo *bi, UWORD start, UWORD count)
{
    struct ChipGPUState *gs = g_chip_state;
    DCHIP_V("SetColorArray(start=%u count=%u)", (unsigned)start, (unsigned)count);
    if (!gs || !bi) return;
    gs->stat_setcolor++;

    /* bi->CLUT is R,G,B byte triplets (3 bytes per entry) */
    for (UWORD i = start; i < start + count && i < 256; i++) {
        UBYTE r = bi->CLUT[i * 3 + 0];
        UBYTE g = bi->CLUT[i * 3 + 1];
        UBYTE b = bi->CLUT[i * 3 + 2];
        /* B8G8R8X8_UNORM: GPU reads memory bytes as B,G,R,X.
         * On PPC BE, uint32 0xBBGGRRXX stores as bytes BB,GG,RR,XX.
         * So GPU reads B=BB, G=GG, R=RR, X=XX -- correct colors with
         * either gl=off (pixman) or gl=on (GL textures). */
        gs->palette[i] = ((uint32)b << 24) | ((uint32)g << 16) | ((uint32)r << 8);
        /* Debug: dump first 4 palette entries to verify byte order */
        if (i < 4 && count > 1)
            DCHIP_V("  palette[%u]: CLUT r=%02x g=%02x b=%02x -> 0x%08lx",
                    (unsigned)i, (unsigned)r, (unsigned)g, (unsigned)b, gs->palette[i]);
    }

    /* If we're in CLUT mode, a palette change needs a display refresh.
     * Flush on batch updates (count>1). Single-entry changes happen
     * during cursor color tweaks -- the next drawing op will flush. */
    if (gs->active_format == RGBFB_CLUT && count > 1) {
        /* Reset one-shot debug so we see the REAL palette conversion */
        gs->clut_debug_done = FALSE;

        chip_flush_signal_activity(gs);

#ifdef CHIP_DEBUG_VERBOSE
        /* Check if board_mem has any content yet (boot image drawn?) */
        {
            UBYTE *bm = (UBYTE *)gs->panning_mem;
            if (bm) {
                uint32 nonzero = 0;
                for (uint32 i = 0; i < 640 && i < gs->active_width; i++) {
                    if (bm[i]) { nonzero = i; break; }
                }
                DCHIP_V("  board_mem check: first_nonzero_idx=%lu byte[0]=%u byte[1]=%u byte[2]=%u byte[3]=%u",
                        nonzero, (unsigned)bm[0], (unsigned)bm[1],
                        (unsigned)bm[2], (unsigned)bm[3]);
            }
        }
        /* Post-flush: dump first 4 pixels from fb_mem to verify conversion */
        if (gs->fb_mem) {
            uint32 *fb = (uint32 *)gs->fb_mem;
            DCHIP_V("  fb_mem after flush: [0]=0x%08lx [1]=0x%08lx [2]=0x%08lx [3]=0x%08lx",
                    fb[0], fb[1], fb[2], fb[3]);
        }
#endif
    }
}

static void chip_SetDAC(struct BoardInfo *bi, RGBFTYPE format)
{
    (void)bi;
    DCHIP_V("SetDAC(format=%ld)", (LONG)format);
}

/* Derive the flush-task idle backstop interval (microseconds) from the
 * active ModeInfo.  refresh_hz = PixelClock / (HorTotal * VerTotal).
 * Clamps the result to 30..240 Hz; falls back to 60 Hz (16667 us) when
 * any of the inputs are zero.  Only used by the flush task's idle Wait;
 * does not cap the active-drawing flush rate. */
static uint32 chip_compute_refresh_us(const struct ModeInfo *mi)
{
    if (!mi || !mi->PixelClock || !mi->HorTotal || !mi->VerTotal)
        return 16667;                       /* 60 Hz fallback */

    ULONG total = (ULONG)mi->HorTotal * (ULONG)mi->VerTotal;
    if (!total) return 16667;

    ULONG refresh_hz = mi->PixelClock / total;
    if (refresh_hz < 30)  refresh_hz = 30;
    if (refresh_hz > 240) refresh_hz = 240;
    return 1000000UL / refresh_hz;
}

static void chip_SetGC(struct BoardInfo *bi, struct ModeInfo *mi, BOOL border)
{
    (void)border;
    if (!mi) return;
    if (g_chip_state) g_chip_state->stat_setgc++;

    RGBFTYPE fmt;
    if (mi->Depth <= 8)       fmt = RGBFB_CLUT;
    else if (mi->Depth <= 16) fmt = RGBFB_R5G6B5;
    else                      fmt = RGBFB_A8R8G8B8;

    /* Recompute refresh_us from the incoming ModeInfo regardless of the
     * no-op path below -- different modes can share dimensions but differ
     * in timing, and the idle-backstop interval must track the real
     * refresh rate reported by PixelClock. */
    if (g_chip_state) {
        uint32 new_refresh_us = chip_compute_refresh_us(mi);
        if (new_refresh_us != g_chip_state->refresh_us) {
            DCHIP_V("SetGC: refresh_us %lu -> %lu (%lu Hz)",
                    (ULONG)g_chip_state->refresh_us, (ULONG)new_refresh_us,
                    (ULONG)(new_refresh_us ? 1000000UL / new_refresh_us : 0));
            g_chip_state->refresh_us = new_refresh_us;
        }
        /* Do NOT write SyncPeriod at +1566 -- that slot is repurposed
         * by AOS4 PCIGraphics.card to hold the PCI Vendor/Device IDs
         * (UWORD pair) for graphics.library v54 GBD_* queries.  See
         * boardinfo.h for the disassembly references.  refresh_us
         * stays internal to ChipGPUState. */
    }

    /* No-op detection: graphics.library/MUI can call SetGC repeatedly with
     * the same mode when opening windows.  Each unnecessary full-frame flush
     * steals CPU and causes visible flicker (especially in double-buffer
     * mode where it forces an out-of-band scanout swap).  If dimensions,
     * format and depth all match the current state, this is a pure re-announce:
     * skip the flush; any pending pixel changes will be presented by the
     * periodic flush task on its next tick (5-20ms). */
    if (g_chip_state &&
        g_chip_state->active_width  == (uint32)mi->Width &&
        g_chip_state->active_height == (uint32)mi->Height &&
        g_chip_state->active_format == fmt) {
        /* Still update bi fields graphics.library expects to be in sync */
        bi->ModeInfo  = mi;
        bi->Depth     = mi->Depth;
        bi->RGBFormat = fmt;
        DCHIP_V("SetGC: no-op, mode unchanged (%ux%u d=%u fmt=%ld)",
                (unsigned)mi->Width, (unsigned)mi->Height,
                (unsigned)mi->Depth, (LONG)fmt);
        return;
    }

    DCHIP("SetGC: %ux%u d=%u fmt=%ld (was %lux%lu fmt=%ld) "
          "bi->Mouse=%d,%d gs->cursor=%d,%d",
          (unsigned)mi->Width, (unsigned)mi->Height, (unsigned)mi->Depth, (LONG)fmt,
          g_chip_state ? (ULONG)g_chip_state->active_width : 0UL,
          g_chip_state ? (ULONG)g_chip_state->active_height : 0UL,
          g_chip_state ? (LONG)g_chip_state->active_format : 0L,
          (int)bi->MouseX, (int)bi->MouseY,
          g_chip_state ? (int)g_chip_state->cursor_x : 0,
          g_chip_state ? (int)g_chip_state->cursor_y : 0);
    if (g_chip_state) {
        struct ChipGPUState *gss = g_chip_state;
        /* Snapshot blit/fill counters on every SetGC so we can compare
         * the per-mode op load (CLUT8 test-screen complaint).  Logs
         * deltas relative to the last SetGC, then resets the snapshot. */
        uint32 d_fr = gss->stat_fillrect      - gss->prev_fillrect;
        uint32 d_br = gss->stat_blitrect      - gss->prev_blitrect;
        uint32 d_nm = gss->stat_blitrect_nmc  - gss->prev_blitrect_nmc;
        uint32 d_bt = gss->stat_blittemplate  - gss->prev_blittemplate;
        uint32 d_bp = gss->stat_blitpattern   - gss->prev_blitpattern;
        DCHIP("SetGC: ops since last SetGC -- "
              "FillRect=%lu BlitRect=%lu NMC=%lu Template=%lu Pattern=%lu",
              (ULONG)d_fr, (ULONG)d_br, (ULONG)d_nm, (ULONG)d_bt, (ULONG)d_bp);
        uint32 dh4   = gss->stat_fr_clut_le4   - gss->prev_fr_clut_le4;
        uint32 dh16  = gss->stat_fr_clut_le16  - gss->prev_fr_clut_le16;
        uint32 dh256 = gss->stat_fr_clut_le256 - gss->prev_fr_clut_le256;
        uint32 dh4k  = gss->stat_fr_clut_le4k  - gss->prev_fr_clut_le4k;
        uint32 dhb   = gss->stat_fr_clut_big   - gss->prev_fr_clut_big;
        if (dh4 + dh16 + dh256 + dh4k + dhb > 0) {
            DCHIP("SetGC: CLUT FillRect area histogram -- "
                  "<=4:%lu <=16:%lu <=256:%lu <=4k:%lu >4k:%lu",
                  (ULONG)dh4, (ULONG)dh16, (ULONG)dh256, (ULONG)dh4k, (ULONG)dhb);
        }
        gss->prev_fillrect      = gss->stat_fillrect;
        gss->prev_blitrect      = gss->stat_blitrect;
        gss->prev_blitrect_nmc  = gss->stat_blitrect_nmc;
        gss->prev_blittemplate  = gss->stat_blittemplate;
        gss->prev_blitpattern   = gss->stat_blitpattern;
        gss->prev_fr_clut_le4   = gss->stat_fr_clut_le4;
        gss->prev_fr_clut_le16  = gss->stat_fr_clut_le16;
        gss->prev_fr_clut_le256 = gss->stat_fr_clut_le256;
        gss->prev_fr_clut_le4k  = gss->stat_fr_clut_le4k;
        gss->prev_fr_clut_big   = gss->stat_fr_clut_big;
    }

    bi->ModeInfo  = mi;
    bi->Depth     = mi->Depth;
    bi->RGBFormat = fmt;

    if (g_chip_state) {
        /* If screen dimensions changed, clear GPU fb outside new active area */
        BOOL dims_changed = (g_chip_state->active_width  != (uint32)mi->Width ||
                             g_chip_state->active_height != (uint32)mi->Height);

        g_chip_state->active_format = fmt;
        g_chip_state->active_width  = mi->Width;
        g_chip_state->active_height = mi->Height;
        UWORD bpp = chip_format_bpp(fmt);
        g_chip_state->active_bpr = (ULONG)mi->Width * bpp;
        /* fb_stride is NEVER changed -- it's the GPU resource stride (max_w * 4) */

        if (dims_changed && g_chip_state->fb_mem) {
            struct ChipGPUState *gs = g_chip_state;
            /* Recreate the framebuffer resource at the new active size.
             * QEMU's gl=on path sizes the SDL window to the resource
             * dimensions, so resource MUST match active for the host
             * window and the AmigaOS rendering to align. */
            if (!chip_resize_fb_to_mode(gs, gs->active_width, gs->active_height)) {
                DCHIP("SetGC: chip_resize_fb_to_mode FAILED -- screen may be stale");
            }
            DCHIP("SetGC: post-resize bi->Mouse=%d,%d gs->cursor=%d,%d active=%lux%lu",
                  (int)bi->MouseX, (int)bi->MouseY,
                  (int)gs->cursor_x, (int)gs->cursor_y,
                  (ULONG)gs->active_width, (ULONG)gs->active_height);
            /* Mark the cursor as needing a position refresh.  AmigaOS
             * rescales bi->MouseX/Y to the new mode (after SetDisplay/
             * SetSwitch), but on the shrink path no SetSpriteImage call
             * follows, so the VirtIO cursor texture never catches up to
             * the new coords.  The next SetSpritePosition call clears
             * this flag by sending one MOVE_CURSOR. */
            gs->cursor_needs_refresh = TRUE;
        }
    }
    chip_flush_signal_activity(g_chip_state);
}

static void chip_SetPanning(struct BoardInfo *bi, APTR mem, UWORD width,
                             WORD xOff, WORD yOff, RGBFTYPE format)
{
    (void)bi;
    DCHIP_V("SetPanning(mem=%p width=%u xOff=%d yOff=%d fmt=%ld)",
            mem, (unsigned)width, (int)xOff, (int)yOff, (LONG)format);
    if (g_chip_state) g_chip_state->stat_setpanning++;
    if (g_chip_state && mem) {
        struct ChipGPUState *gs = g_chip_state;

        /* Detect screen switch -- log at DCHIP level (always-on) when
         * the panning bitmap pointer changes, so we can see exactly which
         * bitmap the flush task will be sampling for the next frame. */
        if (gs->panning_mem != mem) {
            DCHIP("SetPanning: panning_mem %p -> %p (w=%u xOff=%d yOff=%d fmt=%ld)",
                  gs->panning_mem, mem,
                  (unsigned)width, (int)xOff, (int)yOff, (LONG)format);
        }
        /* Detect xOff/yOff drift even when the bitmap pointer is unchanged
         * -- screen-drag and virtual-screen panning shows up here.  Useful
         * to confirm whether AOS4 ever asks us for non-zero offsets, and
         * to spot suspect-large values that would read past the bitmap
         * end (the bitmap height isn't known to the chip; chip_convert_rect
         * trusts pan_yoff and reads from src + pan_y * stride). */
        if ((WORD)gs->pan_xoff != xOff || (WORD)gs->pan_yoff != yOff) {
            DCHIP("SetPanning: pan offsets %d,%d -> %d,%d (mem=%p w=%u)",
                  (int)gs->pan_xoff, (int)gs->pan_yoff,
                  (int)xOff, (int)yOff, mem, (unsigned)width);
        }
        if (gs->panning_width != (uint32)width) {
            DCHIP("SetPanning: panning_width %lu -> %u",
                  (ULONG)gs->panning_width, (unsigned)width);
        }

        gs->panning_mem = mem;
        gs->last_panning_mem = mem;
        gs->panning_width = (uint32)width;
        gs->pan_xoff = xOff;
        gs->pan_yoff = yOff;

        /* Update active format from P96 -- this is the ACTUAL format of the
         * panning surface.  SetGC picks format from depth, but SetPanning is
         * the authoritative source because P96 may choose a different format
         * than what SetGC assumed (e.g. A8R8G8B8 vs B8G8R8A8). */
        if ((ULONG)format != gs->active_format) {
            DCHIP("SetPanning: format changed %ld -> %ld",
                  (LONG)gs->active_format, (LONG)format);
            gs->active_format = (uint32)format;
            UWORD bpp = chip_format_bpp(format);
            gs->active_bpr = (ULONG)width * bpp;
        }
    } else if (!mem) {
        DCHIP("SetPanning: mem=NULL -- panning state NOT updated");
    }
    chip_flush_signal_activity(g_chip_state);
}

/* -----------------------------------------------------------------------
 * Memory/format helper vtable functions
 * ----------------------------------------------------------------------- */
static UWORD chip_CalculateBytesPerRow(struct BoardInfo *bi, UWORD width,
                                        RGBFTYPE format)
{
    (void)bi;
    UWORD bpp = chip_format_bpp(format);
    return (UWORD)((ULONG)width * bpp);
}

static APTR chip_CalculateMemory(struct BoardInfo *bi, ULONG mem, RGBFTYPE format)
{
    (void)bi; (void)format;
    return (APTR)mem;
}

static ULONG chip_GetCompatibleFormats(struct BoardInfo *bi, RGBFTYPE format)
{
    (void)bi;
    ULONG fmts = RGBFF_CLUT | RGBFF_R5G6B5 | RGBFF_B8G8R8A8 | RGBFF_A8R8G8B8;
    DCHIP_V("GetCompatibleFormats(fmt=%ld) -> 0x%lx", (LONG)format, fmts);
    return fmts;
}

static BOOL chip_SetDisplay(struct BoardInfo *bi, BOOL enabled)
{
    DCHIP("SetDisplay(%ld) ModeInfo=%p", (LONG)enabled, bi ? bi->ModeInfo : NULL);

    struct ChipGPUState *gs = g_chip_state;
    if (gs) gs->stat_setdisplay++;

    /* Start flush task at first SetDisplay(1) -- before graphics.library draws
     * the boot logo.  Uses IExec->CreateTaskTags which properly initialises
     * tc_MemEntry (a List field) and all AmigaOS 4 internal Task fields,
     * avoiding DSI crashes inside exec helpers that walk these lists later.
     * Manual AllocVecTags + AddTask leaves tc_MemEntry zero-filled with
     * lh_Head=NULL, and any exec op that touches it (e.g. during task
     * cleanup or internal bookkeeping after ~30s of heavy signal churn)
     * faults with stw r10,4(r9) where r9=NULL -- exactly what we saw in
     * the virtiogpu.flush DSI traps at kernel+0x33538 DAR=0x4.
     *
     * No dos.library needed (that's CreateNewProcTags).  Falls through to
     * the dos-based fallback in chip_SetSwitch only if this route fails. */
    if (enabled && gs && !gs->flush_running) {
        struct ExecIFace *IExec = gs->IExec;

        /* Priority 10 (above default user tasks at 0 and most service
         * tasks).  At pri 5 gfxbench2d's OverlappedBltBitMap(512^2)
         * phase still drops us to 0.04 Hz because the bench's 1MB-per-op
         * memory-copy loop saturates the bus.  Pri 10 guarantees we run
         * the moment the pacing timer fires, regardless of what other
         * pri 0..5 tasks are doing.  The flush task sleeps most of the
         * time so this costs nothing on idle systems. */
        struct Task *task = IExec->CreateTaskTags(
            "virtiogpu.flush",
            10,                               /* priority */
            (CONST_APTR)chip_flush_task_entry,
            32768,                            /* stack size */
            TAG_DONE);
        if (task) {
            gs->flush_running = TRUE;
            DCHIP("Flush task created (CreateTaskTags): %p", task);
        } else {
            DCHIP("WARNING: CreateTaskTags failed for flush task "
                  "(will retry via dos.library in SetSwitch)");
        }
    }

    if (enabled) chip_flush_signal_activity(gs);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * Pixel clock vtable functions
 * ----------------------------------------------------------------------- */
static ULONG chip_ResolvePixelClock(struct BoardInfo *bi, struct ModeInfo *mi,
                                     ULONG pixelClock, RGBFTYPE format)
{
    (void)bi; (void)format;
    if (!mi) return 0;

    /* Virtual GPU -- accept any pixel clock, find closest table entry for index.
     * Start from 1: index 0 is reserved (P96 treats return 0 as "not supported"). */
    ULONG best_idx = 1;
    ULONG best_diff = 0xFFFFFFFF;
    for (ULONG i = 1; i < chip_num_modes; i++) {
        ULONG diff = (pixelClock > chip_modes[i].clock)
                   ? (pixelClock - chip_modes[i].clock)
                   : (chip_modes[i].clock - pixelClock);
        if (diff < best_diff) {
            best_diff = diff;
            best_idx = i;
        }
    }

    /* Accept the EXACT requested clock -- virtual GPU has no PLL constraints */
    mi->PixelClock = pixelClock;
    mi->clock      = (UBYTE)best_idx;
    mi->clock_div  = 1;

    DCHIP_V("  -> idx=%lu accepted clock=%lu", best_idx, pixelClock);
    return best_idx;
}

static ULONG chip_GetPixelClock(struct BoardInfo *bi, struct ModeInfo *mi,
                                 ULONG index, RGBFTYPE format)
{
    (void)bi; (void)mi; (void)format;
    ULONG clk = (index < chip_num_modes) ? chip_modes[index].clock : 0;
    DCHIP_V("GetPixelClock(mi=%p idx=%lu fmt=%ld) -> %lu",
            mi, index, (LONG)format, clk);
    return clk;
}

/* -----------------------------------------------------------------------
 * chip_WaitVerticalSync -- block the calling task until the next vblank.
 *
 * Canonical contract: return after the next falling edge of the chip's
 * vblank signal.  VirtIO GPU has no real vblank IRQ, so we synthesise
 * one off the flush task's refresh-paced wake-up: register the calling
 * task in gs->wait_vsync_task, sleep on a freshly-allocated signal,
 * the flush task picks up the registration on its next tick and signals
 * us awake.
 *
 * Single-slot rendezvous.  In the unlikely event of a second waiter
 * arriving while a first is still parked, the first task remains
 * registered and its signal eventually fires; the second task replaces
 * the slot and races for the same wake-up.  Both wake within one
 * refresh interval of each other.
 *
 * The `beam` parameter is a P96 historical thing -- TRUE means "wait
 * for the actual beam", FALSE means "any vblank-class event will do".
 * On a virtual GPU the distinction is meaningless; ignored.
 * ----------------------------------------------------------------------- */
static void chip_WaitVerticalSync(struct BoardInfo *bi, BOOL beam)
{
    (void)bi; (void)beam;
    struct ChipGPUState *gs = g_chip_state;
    if (!gs || !gs->flush_task || !gs->vsync_mutex) return;

    struct ExecIFace *IExec = gs->IExec;

    int8 sig_bit = IExec->AllocSignal(-1);
    if (sig_bit < 0) {
        /* Caller has exhausted its signal pool -- nothing useful we can
         * do.  Returning matches "vsync hardware unavailable" semantics
         * better than a busy spin would. */
        return;
    }
    uint32 sig_mask = 1UL << sig_bit;
    struct Task *me = IExec->FindTask(NULL);

    /* Publish task + mask under the rendezvous mutex so the flush task
     * either sees both fields populated (and signals) or sees neither
     * (no spurious signal).  AOS4 mutex preferred over Forbid/Permit:
     * only the flush task contends, the rest of the system runs. */
    IExec->MutexObtain(gs->vsync_mutex);
    gs->wait_vsync_task = me;
    gs->wait_vsync_mask = sig_mask;
    IExec->MutexRelease(gs->vsync_mutex);

    /* Block.  The flush task will wake within at most one refresh
     * interval (~16 ms at 60 Hz). */
    IExec->Wait(sig_mask);

    /* Defensive: if we were woken for some other reason and the flush
     * task hasn't claimed the slot yet, retract the registration so
     * the next caller doesn't get a stale signal. */
    IExec->MutexObtain(gs->vsync_mutex);
    if (gs->wait_vsync_task == me) {
        gs->wait_vsync_task = NULL;
        gs->wait_vsync_mask = 0;
    }
    IExec->MutexRelease(gs->vsync_mutex);

    IExec->FreeSignal(sig_bit);
}

/* -----------------------------------------------------------------------
 * chip_GetCurrentY -- synthesised "beam Y" position.
 *
 * Canonical contract: return current vertical beam position (line
 * number).  Apps doing software vsync-based pacing read this in a loop
 * to wait for a specific scanline.  VirtIO GPU has no real beam, but
 * the flush task stamps last_vsync_eclock_lo on every refresh wake-up,
 * so we can fake it by computing how far through the refresh interval
 * we are and scaling to the active height.
 *
 *   delta_us  = (now_eclock - last_vsync_eclock) * 1e6 / eclock_freq
 *   fraction  = delta_us / refresh_us         (clamped to [0..1])
 *   y         = fraction * active_height
 *
 * If ITimer or eclock_freq isn't yet published (flush task not started)
 * we return 0, the canonical "top of frame" answer.
 * ----------------------------------------------------------------------- */
static ULONG chip_GetCurrentY(struct BoardInfo *bi)
{
    (void)bi;
    struct ChipGPUState *gs = g_chip_state;
    if (!gs) return 0;
    struct TimerIFace *ITimer = gs->ITimer;
    uint32 freq = gs->eclock_freq;
    uint32 refresh_us = gs->refresh_us;
    uint32 height = gs->active_height;
    if (!ITimer || !freq || !refresh_us || !height) return 0;

    struct EClockVal ev;
    ITimer->ReadEClock(&ev);
    uint32 last = gs->last_vsync_eclock_lo;
    uint32 delta_ticks = ev.ev_lo - last;       /* unsigned wrap is fine */

    /* Convert ticks -> microseconds without 32-bit overflow.  At 1 MHz
     * eclock and a 16667us refresh, delta_ticks fits in 17 bits, so
     * delta_ticks * 1000000 fits comfortably in 64-bit. */
    uint64 delta_us = ((uint64)delta_ticks * 1000000ULL) / freq;
    if (delta_us >= refresh_us) return height - 1;

    return (ULONG)((delta_us * height) / refresh_us);
}

/* -----------------------------------------------------------------------
 * chip_SetInterrupt -- latch BIF_VBLANKINTERRUPT enable state.
 *
 * Canonical contract: enable/disable VBlank interrupt generation.
 * VirtIO GPU has no VBlank IRQ source we can drive (the host SDL
 * window has no vblank we can observe), so this just stores the
 * requested state for diagnostics.  rtg.library will not call this
 * unless BIF_VBLANKINTERRUPT is advertised in bi->Flags, which we
 * deliberately don't do until WaitTOF queue handling is in place.
 * ----------------------------------------------------------------------- */
static BOOL chip_SetInterrupt(struct BoardInfo *bi, BOOL enable)
{
    (void)bi;
    if (g_chip_state) g_chip_state->vbi_enabled = (UBYTE)(enable ? 1 : 0);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * chip_GetVSyncState -- return the synthetic vblank phase.
 *
 * VirtIO GPU has no vblank register; the flush task toggles
 * gs->vsync_phase on each refresh-paced wakeup (signal-driven or timer
 * backstop), so callers that poll this in a loop see a level change
 * at least once per refresh interval -- enough to detect frame
 * boundaries for animation pacing.  The 'toggle' parameter mirrors
 * the P96 API but is ignored by every chip driver in practice
 * (ZZ9000.card / SM502.chip do the same).
 * ----------------------------------------------------------------------- */
static BOOL chip_GetVSyncState(struct BoardInfo *bi, BOOL toggle)
{
    (void)bi; (void)toggle;
    if (!g_chip_state) return FALSE;
    return (BOOL)(g_chip_state->vsync_phase & 1);
}

static void chip_SetDPMSLevel(struct BoardInfo *bi, ULONG level)
{
    (void)bi;
    DCHIP("SetDPMSLevel(%lu)", level);
    struct ChipGPUState *gs = g_chip_state;
    if (!gs) {
        DCHIP("SetDPMSLevel: g_chip_state NULL -- chip not initialised");
        return;
    }

    if (level > DPMS_OFF) {
        DCHIP("SetDPMSLevel: unknown level %lu (DPMS_ON=0..DPMS_OFF=3); "
              "treating as STANDBY", level);
    }

    gs->dpms_level = level;

    if (level == DPMS_ON) {
        /* Re-enable scanout with current dimensions */
        uint32 w = gs->active_width < gs->fb_width ? gs->active_width : gs->fb_width;
        uint32 h = gs->active_height < gs->fb_height ? gs->active_height : gs->fb_height;
        chip_SetScanout(gs, 0, gs->resource_id, 0, 0, w, h);
        chip_flush_signal_activity(gs);
    } else {
        /* STANDBY/SUSPEND/OFF: disable scanout to blank display */
        chip_SetScanout(gs, 0, 0, 0, 0, 0, 0);
    }
}


/* -----------------------------------------------------------------------
 * Hardware cursor -- VirtIO GPU cursor queue (queue 1).
 *
 * P96 sprite data: bi->MouseImage points to an array of ULONGs.
 * Row 0 is padding (skipped).  For each row 1..MouseHeight:
 *   If BIF_DBLSCANDBLSPRITEY or width > 16:
 *     ULONG[row*2 + 0] = data (32 bits)
 *     ULONG[row*2 + 1] = mask (32 bits)
 *   Else (standard 16-wide Amiga sprite):
 *     UWORD[row*2 + 0] = data (16 bits)
 *     UWORD[row*2 + 1] = mask (16 bits)
 *
 * data=1,mask=1 -> color 2 (fg)
 * data=0,mask=1 -> color 1
 * data=1,mask=0 -> color 3
 * data=0,mask=0 -> transparent
 * ----------------------------------------------------------------------- */
static BOOL chip_SetSprite(struct BoardInfo *bi, BOOL activate, RGBFTYPE format)
{
    (void)bi; (void)format;
    struct ChipGPUState *gs = g_chip_state;
    if (!gs) return FALSE;
    gs->stat_setsprite++;

    /* Track visibility for SetSpritePosition filtering.
     * Do NOT send UPDATE_CURSOR to hide/show -- VirtIO GPU has no cheap
     * visibility toggle (unlike SM502's register bit).  Sending
     * UPDATE_CURSOR(resource_id=0) + UPDATE_CURSOR(resource_id=N) causes
     * visible position flicker because the re-define cycle isn't instant.
     * Keep the hardware cursor always visible instead. */
    gs->cursor_visible = activate;
    return activate;
}

static void chip_SetSpritePosition(struct BoardInfo *bi, WORD x, WORD y,
                                    RGBFTYPE format)
{
    (void)format;
    struct ChipGPUState *gs = g_chip_state;
    if (!gs) return;
    gs->stat_setspritepos++;

    gs->cursor_x = x;
    gs->cursor_y = y;

    /* One-shot MOVE_CURSOR after a SetGC mode change.  AmigaOS rescales
     * its internal mouse coords to the new screen size (e.g.
     * 967x676/1600x1200 -> 791x468/1280x800), but on shrink no
     * SetSpriteImage follows -- so without this push, the VirtIO
     * cursor texture stays at the pre-resize position until the user
     * triggers an image change (right-click, hover, etc.).  Sending
     * MOVE_CURSOR exactly once per SetGC keeps the cursor texture
     * aligned with what AmigaOS thinks the position is, without the
     * per-move flicker that previous always-MOVE_CURSOR attempts hit. */
    if (gs->cursor_needs_refresh && gs->cursor_created &&
        (x != gs->cursor_last_sent_x || y != gs->cursor_last_sent_y))
    {
        chip_CursorMove(gs, x, y);
        gs->cursor_last_sent_x = x;
        gs->cursor_last_sent_y = y;
        gs->cursor_needs_refresh = FALSE;
        DCHIP("SpritePos: post-SetGC refresh -- MOVE_CURSOR(%d,%d)",
              (int)x, (int)y);
    }

    /* Diagnostic: warn when cursor position lands outside the active
     * scanout.  x/y are signed -- negatives (pointer parked off the
     * top/left edge) are normal, not OOB; only flag the right/bottom. */
    if (bi && ((x >= 0 && (uint32)x >= gs->active_width) ||
               (y >= 0 && (uint32)y >= gs->active_height))) {
        if (gs->stat_setspritepos - gs->cursor_oob_last_log > 50) {
            DCHIP("SpritePos: OOB %d,%d vs active=%lux%lu bi->Mouse=%dx%d",
                  (int)x, (int)y,
                  (ULONG)gs->active_width, (ULONG)gs->active_height,
                  (int)bi->MouseX, (int)bi->MouseY);
            gs->cursor_oob_last_log = gs->stat_setspritepos;
        }
    }
}

static void chip_SetSpriteImage(struct BoardInfo *bi, RGBFTYPE format)
{
    (void)format;
    struct ChipGPUState *gs = g_chip_state;
    if (!gs || !bi) return;
    gs->stat_setspriteimg++;

    /* Lazily create cursor GPU resource on first use */
    if (!gs->cursor_created) {
        if (!chip_CursorCreate(gs)) return;
    }

    /* Clear the 64x64 cursor buffer */
    uint32 *cursor = (uint32 *)gs->cursor_mem;
    if (!cursor) return;
    for (uint32 i = 0; i < 64 * 64; i++)
        cursor[i] = 0;

    UBYTE mw = bi->MouseWidth;
    UBYTE mh = bi->MouseHeight;
    if (mw == 0 || mh == 0) return;
    if (mw > 64) mw = 64;
    if (mh > 64) mh = 64;

    gs->cursor_w = mw;
    gs->cursor_h = mh;

    /* Build ARGB colors from sprite palette:
     * GPU format B8G8R8A8_UNORM: on PPC BE, uint32 = 0xBBGGRRAA
     * color 0 = transparent (mask=0, data=0)
     * color 1 = sprite color 0 (mask=1, data=0)
     * color 2 = sprite color 1 (mask=1, data=1)  -- usually the main cursor color
     * color 3 = sprite color 2 (mask=0, data=1) */
    uint32 colors[4];
    colors[0] = 0x00000000;  /* transparent */
    for (int c = 0; c < 3; c++) {
        UBYTE r = gs->cursor_colors[c][0];
        UBYTE g = gs->cursor_colors[c][1];
        UBYTE b = gs->cursor_colors[c][2];
        /* B8G8R8A8_UNORM: byte[0]=B, byte[1]=G, byte[2]=R, byte[3]=A
         * PPC BE uint32: 0xBBGGRRFF */
        colors[c + 1] = ((uint32)b << 24) | ((uint32)g << 16) |
                         ((uint32)r << 8) | 0xFF;
    }

    /* Convert P96 sprite data -> 64x64 ARGB.
     * Format depends on mouseWidth: <=16 = UWORD pairs (4 bytes/row),
     * >16 = ULONG pairs (8 bytes/row).  Row 0 is a header, data starts at row 1.
     * MSB = leftmost pixel in each word. */
    UBYTE *mouseImage = (UBYTE *)bi->MouseImage;
    if (!mouseImage) return;

    BOOL wide = (mw > 16);

    for (UBYTE row = 0; row < mh; row++) {
        uint32 data_bits, mask_bits;

        if (wide) {
            /* 32-bit source: ULONG data + ULONG mask per row */
            uint32 *src = (uint32 *)(mouseImage + (uint32)(row + 1) * 8);
            data_bits = src[0];
            mask_bits = src[1];
        } else {
            /* 16-bit source: UWORD data + UWORD mask per row */
            uint16 *src = (uint16 *)(mouseImage + (uint32)(row + 1) * 4);
            data_bits = (uint32)src[0] << 16;
            mask_bits = (uint32)src[1] << 16;
        }

        uint32 *dst_row = cursor + (uint32)row * 64;
        UBYTE bits = wide ? 32 : 16;
        if (bits > mw) bits = mw;

        for (UBYTE col = 0; col < bits; col++) {
            uint32 bit = 1U << (31 - col);
            UBYTE d = (data_bits & bit) ? 1 : 0;
            UBYTE m = (mask_bits & bit) ? 1 : 0;
            /* Standard Amiga sprite: color = (mask << 1) | data
             * 0=transparent, 1=color0, 2=color1, 3=color2 */
            UBYTE cidx = (m << 1) | d;
            dst_row[col] = colors[cidx];
        }
    }

    /* Sync position from BoardInfo so UPDATE_CURSOR has current coordinates.
     * P96 sets MouseX/MouseY before calling SetSpriteImage. */
    gs->cursor_x     = bi->MouseX;
    gs->cursor_y     = bi->MouseY;
    gs->cursor_hot_x = bi->MouseXOffset;
    gs->cursor_hot_y = bi->MouseYOffset;

    DCHIP("SpriteImage: %ux%u hot=%d,%d pos=%d,%d active=%lux%lu",
          (unsigned)mw, (unsigned)mh,
          (int)bi->MouseXOffset, (int)bi->MouseYOffset,
          (int)bi->MouseX, (int)bi->MouseY,
          (ULONG)gs->active_width, (ULONG)gs->active_height);

    /* Upload cursor image with current position and hotspot */
    chip_CursorUpdate(gs, (uint32)bi->MouseXOffset, (uint32)bi->MouseYOffset);
}

static void chip_SetSpriteColor(struct BoardInfo *bi, UBYTE idx,
                                 UBYTE r, UBYTE g, UBYTE b, RGBFTYPE format)
{
    (void)bi; (void)format;
    struct ChipGPUState *gs = g_chip_state;
    if (!gs || idx > 2) return;
    gs->stat_setspritecol++;

    gs->cursor_colors[idx][0] = r;
    gs->cursor_colors[idx][1] = g;
    gs->cursor_colors[idx][2] = b;
}



/* -----------------------------------------------------------------------
 * chip_fill_boardinfo_vtable -- assign all P96 function pointers.
 * Called from chip_InitCard_C after BoardInfo fields are filled.
 * ----------------------------------------------------------------------- */
void chip_fill_boardinfo_vtable(struct BoardInfo *bi)
{
    /* Slots owned by chip_alloc.c: AllocCardMem, FreeCardMem,
     * AllocBitMap, FreeBitMap, GetBitMapAttr (slots 0, 1, 58, 59, 60). */
    chip_alloc_fill_vtable(bi);

    /* Slots owned by chip_blit.c: BlitPlanar2Chunky/FB,
     * BlitPlanar2Direct/FB, FillRect/FB, InvertRect/FB, BlitRect/FB,
     * BlitTemplate/FB, BlitPattern/FB, DrawLine/FB,
     * BlitRectNoMaskComplete/FB. */
    chip_blit_fill_vtable(bi);

    /* Display configuration / pixel clock / vsync / sprite -- the
     * functions still defined in this file. */
    bi->SetSwitch       = chip_SetSwitch;
    bi->SetColorArray   = chip_SetColorArray;
    bi->SetDAC          = chip_SetDAC;
    bi->SetGC           = chip_SetGC;
    bi->SetPanning      = chip_SetPanning;
    bi->CalculateBytesPerRow = chip_CalculateBytesPerRow;
    bi->CalculateMemory = chip_CalculateMemory;
    bi->GetCompatibleFormats = chip_GetCompatibleFormats;
    bi->SetDisplay      = chip_SetDisplay;
    bi->ResolvePixelClock = chip_ResolvePixelClock;
    bi->GetPixelClock   = chip_GetPixelClock;
    bi->SetClock        = (void (*)(struct BoardInfo *))stub_SetClock;
    bi->SetMemoryMode   = (void (*)(struct BoardInfo *, RGBFTYPE))stub_SetMemoryMode;
    bi->SetWriteMask    = (void (*)(struct BoardInfo *, UBYTE))stub_SetWriteMask;
    bi->SetClearMask    = (void (*)(struct BoardInfo *, UBYTE))stub_SetClearMask;
    bi->SetReadPlane    = (void (*)(struct BoardInfo *, UBYTE))stub_SetReadPlane;
    bi->WaitVerticalSync = chip_WaitVerticalSync;
    bi->SetInterrupt    = chip_SetInterrupt;
    bi->WaitBlitter     = (void (*)(struct BoardInfo *))stub_WaitBlitter;
    bi->_fp21           = stub_fp21;
    bi->_fp22           = stub_fp22;
    bi->_fp23           = stub_fp23;
    bi->_fp24           = stub_fp24;
    bi->_fp43           = stub_fp43;
    bi->_fp44 = stub_fp44; bi->_fp45 = stub_fp45; bi->_fp46 = stub_fp46;
    bi->_fp47 = stub_fp47; bi->_fp48 = stub_fp48; bi->_fp49 = stub_fp49;
    bi->_fp50 = stub_fp50; bi->_fp51 = stub_fp51; bi->_fp52 = stub_fp52;
    bi->GetVSyncState  = chip_GetVSyncState;
    bi->GetCurrentY    = chip_GetCurrentY;
    bi->SetDPMSLevel   = chip_SetDPMSLevel;
    bi->ResetChip      = (void (*)(struct BoardInfo *))stub_ResetChip;
    bi->_fp57          = stub_fp57;
    bi->SetSprite      = chip_SetSprite;
    bi->SetSpritePosition = chip_SetSpritePosition;
    bi->SetSpriteImage = chip_SetSpriteImage;
    bi->SetSpriteColor = chip_SetSpriteColor;
    bi->_fp65 = stub_fp65; bi->_fp66 = stub_fp66; bi->_fp67 = stub_fp67;
}
