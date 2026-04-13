/*
 * chip_blit.c -- Picasso96 blitting + drawing vtable callbacks.
 * Split from chip_p96.c in v53.112.
 *
 * Provides the CPU implementations (with Virgl GPU acceleration where
 * applicable) for:
 *   - chip_FillRect / chip_InvertRect / chip_BlitRect
 *   - chip_BlitRectNoMaskComplete (slot 39, with full Minterm decode)
 *   - chip_BlitTemplate / chip_BlitPattern (1bpp expand with FG/BG/DrawMode)
 *   - chip_DrawLine (Bresenham with line pattern)
 *   - chip_BlitPlanar2Chunky / chip_BlitPlanar2Direct (planar -> RTG)
 *   - chip_blit_fill_vtable() to wire everything into BoardInfo.
 *
 * Display-config callbacks (SetSwitch, SetGC, SetPanning, SetSprite*,
 * vsync) stay in chip_p96.c because they share state with the
 * mode/refresh machinery.  Allocator callbacks live in chip_alloc.c.
 */

#include "chip/chip_state.h"

/* DrawMode constants -- same encoding as graphics.library RastPort. */
#define JAM1       0
#define JAM2       1
#define COMPLEMENT 2

/* -----------------------------------------------------------------------
 * Fast row-level helpers used by the blit/fill paths.
 *
 * gfxbench2d's OverlappedBltBitMap test issued ~512x512 32bpp blits in
 * tight loops; the original byte-at-a-time copy ran at ~100 MB/s on PPC
 * G4 and dominated the benchmark.  Routes:
 *
 *   non-overlapping, longword-aligned: IExec->CopyMemQuick   (lwz/stw burst)
 *   non-overlapping, any alignment:    IExec->CopyMem        (general copy)
 *   overlapping (same buffer):         hand-rolled long-word memmove with
 *                                      forward/reverse direction
 *
 * We can't use newlib's memcpy/memmove/memset because the chip is built
 * with -D__NOLIBBASE__ -nostartfiles, so INewlib is never opened and any
 * call to newlib lowers to a stub that calls a NULL interface vector.
 * The exec primitives are always available via gs->IExec.
 *
 * `chip_copy_rect` handles overlap implicitly: caller supplies `overlap`
 * = TRUE iff src and dst alias.  When overlap=FALSE we use CopyMemQuick
 * per row when alignment permits, else CopyMem.
 * ----------------------------------------------------------------------- */

/* Long-word memmove for an overlapping single row.  src/dst must be
 * 4-byte aligned and len must be a multiple of 4 (always true for our
 * 32bpp blits; row_bytes = w * 4).  Direction is chosen per `forward`. */
static inline void chip_lmemmove(const ULONG *src, ULONG *dst, ULONG longs,
                                   BOOL forward)
{
    if (forward) {
        for (ULONG i = 0; i < longs; i++) dst[i] = src[i];
    } else {
        for (ULONG i = longs; i > 0; i--) dst[i - 1] = src[i - 1];
    }
}

/* Per-row copy chooser.  When aligned and len%4==0 use CopyMemQuick (fastest);
 * otherwise CopyMem.  Caller guarantees no per-row overlap (rows are independent
 * regardless of vertical direction; per-row src/dst are non-overlapping unless
 * dx/x change inside a same-bitmap dy==y row -- chip_BlitRect handles that case
 * via the overlap path). */
static inline void chip_row_copy(struct ExecIFace *IExec,
                                   const UBYTE *src, UBYTE *dst, ULONG len)
{
    /* PPC pointers from board_mem allocations are 4K aligned and our
     * row offsets land on 4-byte boundaries for any sensible bpp -- but
     * not all callers may guarantee both, so test cheaply. */
    if (((((ULONG)src) | ((ULONG)dst) | len) & 3UL) == 0UL) {
        IExec->CopyMemQuick((CONST_APTR)src, (APTR)dst, len);
    } else {
        IExec->CopyMem((CONST_APTR)src, (APTR)dst, len);
    }
}

static inline void chip_copy_rect(struct ExecIFace *IExec,
                                   const UBYTE *src, ULONG s_stride,
                                   UBYTE *dst, ULONG d_stride,
                                   ULONG row_bytes, UWORD h)
{
    if (row_bytes == 0 || h == 0) return;
    for (UWORD row = 0; row < h; row++) {
        chip_row_copy(IExec,
                      src + (ULONG)row * s_stride,
                      dst + (ULONG)row * d_stride,
                      row_bytes);
    }
}

/* Long-word fill for the common 32bpp / 8bpp paths.  For 8bpp the
 * `pattern` is the byte value broadcast to all four bytes; for 32bpp
 * it is the pixel.  Tail bytes are filled byte-at-a-time when len is
 * not a multiple of 4 (rare on RTG modes). */
static inline void chip_lfill(UBYTE *dst, ULONG row_bytes, ULONG pattern)
{
    ULONG i = 0;
    if ((((ULONG)dst) & 3UL) == 0UL) {
        ULONG longs = row_bytes >> 2;
        ULONG *dl = (ULONG *)dst;
        for (ULONG j = 0; j < longs; j++) dl[j] = pattern;
        i = longs << 2;
    }
    UBYTE bytepat = (UBYTE)pattern;
    for (; i < row_bytes; i++) dst[i] = bytepat;
}

static inline void chip_fill_rect_bytes(UBYTE *dst, ULONG d_stride,
                                         ULONG row_bytes, UWORD h, UBYTE val)
{
    if (row_bytes == 0 || h == 0) return;
    /* Broadcast the byte value to all 4 lanes for the long-word inner loop. */
    ULONG pat = ((ULONG)val << 24) | ((ULONG)val << 16)
              | ((ULONG)val << 8)  |  (ULONG)val;
    for (UWORD row = 0; row < h; row++)
        chip_lfill(dst + (ULONG)row * d_stride, row_bytes, pat);
}

/* XOR rop fast path -- 32 bits at a time when alignment permits.
 * For 32bpp formats row_bytes is always a multiple of 4 and the row
 * pointer is naturally 4-byte aligned, so we end up doing 4-byte
 * XORs the whole row.  Falls back to byte XOR for any tail. */
static inline void chip_xor_rect_with_src(const UBYTE *src, ULONG s_stride,
                                            UBYTE *dst, ULONG d_stride,
                                            ULONG row_bytes, UWORD h)
{
    for (UWORD row = 0; row < h; row++) {
        const UBYTE *s = src + (ULONG)row * s_stride;
        UBYTE *d = dst + (ULONG)row * d_stride;
        ULONG i = 0;
        if ((((ULONG)s | (ULONG)d) & 3UL) == 0UL) {
            ULONG longs = row_bytes >> 2;
            ULONG *dl = (ULONG *)d;
            const ULONG *sl = (const ULONG *)s;
            for (ULONG j = 0; j < longs; j++) dl[j] ^= sl[j];
            i = longs << 2;
        }
        for (; i < row_bytes; i++) d[i] ^= s[i];
    }
}

static inline void chip_xor_rect_with_const(UBYTE *dst, ULONG d_stride,
                                              ULONG row_bytes, UWORD h)
{
    /* All-1 XOR (DstInvert).  Same alignment trick as the src variant. */
    for (UWORD row = 0; row < h; row++) {
        UBYTE *d = dst + (ULONG)row * d_stride;
        ULONG i = 0;
        if (((ULONG)d & 3UL) == 0UL) {
            ULONG longs = row_bytes >> 2;
            ULONG *dl = (ULONG *)d;
            for (ULONG j = 0; j < longs; j++) dl[j] ^= 0xFFFFFFFFUL;
            i = longs << 2;
        }
        for (; i < row_bytes; i++) d[i] ^= 0xFFU;
    }
}

/* -----------------------------------------------------------------------
 * chip_BlitPlanar2Chunky -- vtable slot 25.
 * Reads bits from bm->Planes[] and assembles palette indices.
 * `mask` is plane-enable; `layer` is unused.
 * ----------------------------------------------------------------------- */
static void chip_BlitPlanar2Chunky(struct BoardInfo *bi, struct BitMap *bm,
                                    struct RenderInfoChip *ri,
                                    WORD x, WORD y, WORD dx, WORD dy,
                                    UWORD w, UWORD h, UBYTE mask, UBYTE layer)
{
    (void)bi; (void)layer;
    if (!bm || !ri || !ri->Memory || w == 0 || h == 0) return;

    UBYTE  depth     = bm->Depth;
    UWORD  src_bpr   = bm->BytesPerRow;
    ULONG  dst_stride = ri->BytesPerRow;
    UBYTE *dst_base   = (UBYTE *)ri->Memory + (ULONG)dy * dst_stride + (ULONG)dx;

    for (UWORD row = 0; row < h; row++) {
        UBYTE *dst_row = dst_base + (ULONG)row * dst_stride;
        for (UWORD col = 0; col < w; col++) {
            uint32 src_x = (uint32)x + col;
            uint32 byte_off = src_x >> 3;
            UBYTE  bit_pos  = 7 - (src_x & 7);
            uint32 src_y_off = ((uint32)y + row) * src_bpr + byte_off;
            UBYTE  pixel = 0;

            for (UBYTE p = 0; p < depth && p < 8; p++) {
                if (!(mask & (1 << p))) continue;
                UBYTE *plane = (UBYTE *)bm->Planes[p];
                if (!plane) continue;
                if (plane[src_y_off] & (1 << bit_pos))
                    pixel |= (1 << p);
            }
            dst_row[col] = pixel;
        }
    }

    chip_flush(dx, dy, w, h);
}

/* -----------------------------------------------------------------------
 * chip_BlitPlanar2Direct -- vtable slot 41.
 *
 * Same plane-scan logic as BlitPlanar2Chunky, but each computed palette
 * index is mapped through clut->Colors[] to a 32-bit ARGB direct color
 * before being written at the destination's native depth.
 *
 * Minterm is ignored: every observed caller uses src copy (0xC0), and
 * ZZ9000.card's reference implementation always does src copy regardless.
 * Implementing every Porter-Duff minterm here would be a lot of code
 * for a path that's almost never taken with anything else.
 * ----------------------------------------------------------------------- */
static void chip_BlitPlanar2Direct(struct BoardInfo *bi, struct BitMap *bm,
                                    struct RenderInfoChip *ri,
                                    struct ColorIndexMapping *clut,
                                    WORD x, WORD y, WORD dx, WORD dy,
                                    WORD w, WORD h, UBYTE minterm, UBYTE mask)
{
    (void)bi;
    if (!bm || !ri || !ri->Memory || !clut || w <= 0 || h <= 0) return;

    /* Log non-SrcCopy minterms once per distinct value -- canonical
     * usage is always 0xC0 (src copy); if we ever see something else
     * the chip will silently fall back to default rtg.library handling
     * and the caller should know. */
    if (minterm != 0xC0) {
        static volatile uint32 bp2d_warned[8];
        BOOL seen = FALSE;
        for (int i = 0; i < 8; i++)
            if (bp2d_warned[i] == minterm) { seen = TRUE; break; }
        if (!seen) {
            for (int i = 0; i < 8; i++) {
                if (bp2d_warned[i] == 0) { bp2d_warned[i] = minterm; break; }
            }
            DCHIP("BlitPlanar2Direct: minterm 0x%02x ignored (only 0xC0 "
                  "implemented; default rtg.library handler will run)",
                  (unsigned)minterm);
        }
    }

    UBYTE  depth      = bm->Depth;
    UWORD  src_bpr    = bm->BytesPerRow;
    ULONG  dst_stride = ri->BytesPerRow;
    UBYTE  bpp        = chip_format_bpp(ri->RGBFormat);
    ULONG  cmask      = clut->ColorMask;
    UBYTE *dst_base   = (UBYTE *)ri->Memory + (ULONG)dy * dst_stride
                                            + (ULONG)dx * bpp;

    for (UWORD row = 0; row < (UWORD)h; row++) {
        UBYTE *dst_row = dst_base + (ULONG)row * dst_stride;
        for (UWORD col = 0; col < (UWORD)w; col++) {
            uint32 src_x = (uint32)x + col;
            uint32 byte_off = src_x >> 3;
            UBYTE  bit_pos  = 7 - (src_x & 7);
            uint32 src_y_off = ((uint32)y + row) * src_bpr + byte_off;
            UBYTE  pixel = 0;

            for (UBYTE p = 0; p < depth && p < 8; p++) {
                if (!(mask & (1 << p))) continue;
                UBYTE *plane = (UBYTE *)bm->Planes[p];
                if (!plane) continue;
                if (plane[src_y_off] & (1 << bit_pos))
                    pixel |= (1 << p);
            }

            ULONG color = clut->Colors[pixel] & cmask;
            if (bpp == 4) {
                /* X8R8G8B8: byte order [00, RR, GG, BB] on PPC BE matches
                 * uint32 0x00RRGGBB -- direct store. */
                *(ULONG *)(dst_row + (ULONG)col * 4) = color;
            } else if (bpp == 2) {
                ULONG r = (color >> 19) & 0x1F;
                ULONG g = (color >> 10) & 0x3F;
                ULONG b = (color >>  3) & 0x1F;
                *(UWORD *)(dst_row + (ULONG)col * 2) =
                    (UWORD)((r << 11) | (g << 5) | b);
            } else {
                /* 8bpp CLUT destination: pass through the index. */
                dst_row[col] = pixel;
            }
        }
    }

    chip_flush(dx, dy, w, h);
}

/* -----------------------------------------------------------------------
 * chip_BlitTemplate -- 1bpp template expand with FgPen/BgPen/DrawMode.
 *
 * DrawMode: JAM1 = fg where bit=1; JAM2 = fg where 1, bg where 0;
 *           COMPLEMENT = XOR dest where bit=1.
 * Template is big-endian bitstream, XOffset = starting bit in first byte.
 * ----------------------------------------------------------------------- */
static void chip_BlitTemplate(struct BoardInfo *bi, struct RenderInfoChip *ri,
                               struct ChipTemplate *tmpl,
                               WORD x, WORD y, UWORD w, UWORD h,
                               UBYTE mask, RGBFTYPE format)
{
    (void)bi; (void)mask;
    if (!ri || !ri->Memory || !tmpl || !tmpl->Memory || w == 0 || h == 0)
        return;
    if (g_chip_state) g_chip_state->stat_blittemplate++;

    UWORD  bpp       = chip_format_bpp(format);
    ULONG  dst_stride = ri->BytesPerRow;
    UBYTE *dst_base   = (UBYTE *)ri->Memory + (ULONG)y * dst_stride + (ULONG)x * bpp;
    UBYTE *src_base   = (UBYTE *)tmpl->Memory;
    WORD   src_stride = tmpl->BytesPerRow;
    UBYTE  xoff       = tmpl->XOffset;
    UBYTE  drawmode   = tmpl->DrawMode;
    ULONG  fg         = tmpl->FgPen;
    ULONG  bg         = tmpl->BgPen;

    for (UWORD row = 0; row < h; row++) {
        UBYTE *src_row = src_base + (LONG)row * src_stride;
        UBYTE *dst_row = dst_base + (ULONG)row * dst_stride;

        for (UWORD col = 0; col < w; col++) {
            uint32 bit_idx = (uint32)xoff + col;
            UBYTE  byte_val = src_row[bit_idx >> 3];
            BOOL   bit_set  = (byte_val >> (7 - (bit_idx & 7))) & 1;

            if (format == RGBFB_CLUT) {
                UBYTE *p = dst_row + col;
                if (drawmode & COMPLEMENT) {
                    if (bit_set) *p ^= 0xFF;
                } else if ((drawmode & 0x03) == JAM2) {
                    *p = bit_set ? (UBYTE)fg : (UBYTE)bg;
                } else { /* JAM1 */
                    if (bit_set) *p = (UBYTE)fg;
                }
            } else if (bpp == 2) {
                UWORD *p = (UWORD *)(dst_row + (ULONG)col * 2);
                if (drawmode & COMPLEMENT) {
                    if (bit_set) *p ^= 0xFFFF;
                } else if ((drawmode & 0x03) == JAM2) {
                    *p = bit_set ? (UWORD)fg : (UWORD)bg;
                } else {
                    if (bit_set) *p = (UWORD)fg;
                }
            } else {
                ULONG *p = (ULONG *)(dst_row + (ULONG)col * bpp);
                if (drawmode & COMPLEMENT) {
                    if (bit_set) *p ^= 0xFFFFFFFF;
                } else if ((drawmode & 0x03) == JAM2) {
                    *p = bit_set ? fg : bg;
                } else {
                    if (bit_set) *p = fg;
                }
            }
        }
    }

    chip_flush(x, y, w, h);
}

/* -----------------------------------------------------------------------
 * chip_BlitPattern -- 16-pixel-wide pattern with 2^N rows + DrawMode.
 * ----------------------------------------------------------------------- */
static void chip_BlitPattern(struct BoardInfo *bi, struct RenderInfoChip *ri,
                              struct ChipPattern *pat,
                              WORD x, WORD y, UWORD w, UWORD h,
                              UBYTE mask, RGBFTYPE format)
{
    (void)bi; (void)mask;
    if (!ri || !ri->Memory || !pat || !pat->Memory || w == 0 || h == 0)
        return;
    if (g_chip_state) g_chip_state->stat_blitpattern++;

    UWORD  bpp       = chip_format_bpp(format);
    ULONG  dst_stride = ri->BytesPerRow;
    UBYTE *dst_base   = (UBYTE *)ri->Memory + (ULONG)y * dst_stride + (ULONG)x * bpp;
    UBYTE *pat_base   = (UBYTE *)pat->Memory;
    UWORD  pat_h      = 1 << pat->Size;
    UBYTE  drawmode   = pat->DrawMode;
    ULONG  fg         = pat->FgPen;
    ULONG  bg         = pat->BgPen;
    UWORD  xoff       = pat->XOffset;
    UWORD  yoff       = pat->YOffset;

    for (UWORD row = 0; row < h; row++) {
        UBYTE *dst_row = dst_base + (ULONG)row * dst_stride;
        UWORD pat_row = ((UWORD)y + row + yoff) & (pat_h - 1);
        UWORD pat_word = ((UWORD)pat_base[pat_row * 2] << 8) |
                          (UWORD)pat_base[pat_row * 2 + 1];

        for (UWORD col = 0; col < w; col++) {
            UWORD pat_col = ((UWORD)x + col + xoff) & 15;
            BOOL bit_set = (pat_word >> (15 - pat_col)) & 1;

            if (format == RGBFB_CLUT) {
                UBYTE *p = dst_row + col;
                if (drawmode & COMPLEMENT) {
                    if (bit_set) *p ^= 0xFF;
                } else if ((drawmode & 0x03) == JAM2) {
                    *p = bit_set ? (UBYTE)fg : (UBYTE)bg;
                } else {
                    if (bit_set) *p = (UBYTE)fg;
                }
            } else if (bpp == 2) {
                UWORD *p = (UWORD *)(dst_row + (ULONG)col * 2);
                if (drawmode & COMPLEMENT) {
                    if (bit_set) *p ^= 0xFFFF;
                } else if ((drawmode & 0x03) == JAM2) {
                    *p = bit_set ? (UWORD)fg : (UWORD)bg;
                } else {
                    if (bit_set) *p = (UWORD)fg;
                }
            } else {
                ULONG *p = (ULONG *)(dst_row + (ULONG)col * bpp);
                if (drawmode & COMPLEMENT) {
                    if (bit_set) *p ^= 0xFFFFFFFF;
                } else if ((drawmode & 0x03) == JAM2) {
                    *p = bit_set ? fg : bg;
                } else {
                    if (bit_set) *p = fg;
                }
            }
        }
    }
    chip_flush(x, y, w, h);
}

/* -----------------------------------------------------------------------
 * chip_DrawLine -- Bresenham with line pattern + DrawMode.
 *
 * The P96 Line struct provides pre-computed Bresenham parameters:
 *   sDelta (short axis abs), lDelta (long axis abs),
 *   twoSDminusLD (initial error term = 2*sDelta - lDelta),
 *   Horizontal (TRUE if X is the long axis).
 * LinePtrn is a 16-bit repeating pattern (0xFFFF = solid).
 * ----------------------------------------------------------------------- */
static void chip_DrawLine(struct BoardInfo *bi, struct RenderInfoChip *ri,
                           APTR lineptr, UBYTE mask, RGBFTYPE format)
{
    (void)bi; (void)mask;
    struct ChipLine *line = (struct ChipLine *)lineptr;
    if (!ri || !ri->Memory || !line) return;
    if (g_chip_state) g_chip_state->stat_drawline++;

    UWORD  bpp    = chip_format_bpp(format);
    ULONG  stride = ri->BytesPerRow;
    UBYTE *base   = (UBYTE *)ri->Memory;
    UBYTE  dm     = line->DrawMode;
    ULONG  fg     = line->FgPen;
    ULONG  bg     = line->BgPen;
    UWORD  ptrn   = line->LinePtrn;
    UWORD  pshift = line->PatternShift;

    WORD px = line->X;
    WORD py = line->Y;
    WORD dx_step = (line->dX >= 0) ? 1 : -1;
    WORD dy_step = (line->dY >= 0) ? 1 : -1;
    WORD sd = line->sDelta;
    WORD ld = line->lDelta;
    WORD err = line->twoSDminusLD;
    UWORD len = line->Length;

    WORD min_x = px, max_x = px;
    WORD min_y = py, max_y = py;

    for (UWORD i = 0; i < len; i++) {
        BOOL draw = (ptrn >> (15 - (pshift & 15))) & 1;
        pshift++;

        if (draw) {
            UBYTE *pixel = base + (ULONG)py * stride + (ULONG)px * bpp;
            if (format == RGBFB_CLUT) {
                if (dm & COMPLEMENT) *pixel ^= 0xFF;
                else                 *pixel = (UBYTE)fg;
            } else if (bpp == 2) {
                UWORD *p = (UWORD *)pixel;
                if (dm & COMPLEMENT) *p ^= 0xFFFF;
                else                 *p = (UWORD)fg;
            } else {
                ULONG *p = (ULONG *)pixel;
                if (dm & COMPLEMENT) *p ^= 0xFFFFFFFF;
                else                 *p = fg;
            }
        } else if ((dm & 0x03) == JAM2) {
            /* Pattern gap in JAM2 draws bg */
            UBYTE *pixel = base + (ULONG)py * stride + (ULONG)px * bpp;
            if (format == RGBFB_CLUT)      *pixel = (UBYTE)bg;
            else if (bpp == 2)             *(UWORD *)pixel = (UWORD)bg;
            else                           *(ULONG *)pixel = bg;
        }

        if (line->Horizontal) {
            px += dx_step;
            if (err > 0) { py += dy_step; err -= 2 * ld; }
            err += 2 * sd;
        } else {
            py += dy_step;
            if (err > 0) { px += dx_step; err -= 2 * ld; }
            err += 2 * sd;
        }

        if (px < min_x) min_x = px;
        if (px > max_x) max_x = px;
        if (py < min_y) min_y = py;
        if (py > max_y) max_y = py;
    }

    chip_flush(min_x, min_y,
               (UWORD)(max_x - min_x + 1),
               (UWORD)(max_y - min_y + 1));
}

/* -----------------------------------------------------------------------
 * chip_FillRect -- vtable slot 27.
 * CPU fill of board_mem (always, for GRANTDIRECTACCESS consistency)
 * with optional Virgl GPU CLEAR fast path on the active scanout.
 * ----------------------------------------------------------------------- */
static void chip_FillRect(struct BoardInfo *bi, struct RenderInfoChip *ri,
                           WORD x, WORD y, UWORD w, UWORD h,
                           ULONG color, UBYTE mask, RGBFTYPE format)
{
    (void)bi; (void)mask;
    if (!ri || !ri->Memory || w == 0 || h == 0) return;
    if (g_chip_state) {
        g_chip_state->stat_fillrect++;
        g_chip_state->last_fill_dst = ri->Memory;
        /* CLUT-test investigation: bucket FillRect rect sizes so we can
         * tell whether the slow CLUT8 case is many tiny rects (color
         * quantization decomposition) or fewer larger rects. */
        if (format == RGBFB_CLUT) {
            ULONG area = (ULONG)w * (ULONG)h;
            if      (area <=    4) g_chip_state->stat_fr_clut_le4++;
            else if (area <=   16) g_chip_state->stat_fr_clut_le16++;
            else if (area <=  256) g_chip_state->stat_fr_clut_le256++;
            else if (area <= 4096) g_chip_state->stat_fr_clut_le4k++;
            else                   g_chip_state->stat_fr_clut_big++;
        }
    }

    UWORD  bpp    = chip_format_bpp(format);
    ULONG  stride = ri->BytesPerRow;
    UBYTE *base   = (UBYTE *)ri->Memory + (ULONG)y * stride + (ULONG)x * bpp;

    CHIP_PROF_DEFINE(prof_fillrect, "FillRect", 200);
    uint32 prof_t0 = chip_prof_begin(g_chip_state);

    if (format == RGBFB_CLUT) {
        /* 8bpp: every byte is the same value -- use memset, which on
         * PPC reduces to long-word stb-broadcast loops in newlib. */
        chip_fill_rect_bytes(base, stride, (ULONG)w, h, (UBYTE)color);
    } else if (bpp == 2) {
        UWORD c16 = (UWORD)color;
        for (UWORD row = 0; row < h; row++) {
            UWORD *p = (UWORD *)(base + (ULONG)row * stride);
            for (UWORD col = 0; col < w; col++) p[col] = c16;
        }
    } else {
        /* 32bpp: long-word stores -- direct uint32 loop; the compiler
         * unrolls these to vectorised stw bursts at -O2. */
        for (UWORD row = 0; row < h; row++) {
            ULONG *p = (ULONG *)(base + (ULONG)row * stride);
            for (UWORD col = 0; col < w; col++) p[col] = color;
        }
    }

    chip_prof_end(g_chip_state, &prof_fillrect, prof_t0,
                  (uint32)((ULONG)w * h * bpp));

    /* Virgl path: GPU-side fill via CLEAR, skip convert+transfer overhead */
    struct ChipGPUState *gs = g_chip_state;
    if (gs && gs->virgl_2d_ready &&
        ri->Memory == gs->board_mem)  /* only accelerate active screen */
    {
        if (chip_virgl_fill_rect(gs, (uint32)x, (uint32)y, w, h, color, format)) {
            chip_flush_signal_activity(gs);
            return;
        }
    }

    chip_flush(x, y, w, h);
}

/* -----------------------------------------------------------------------
 * chip_BlitRect -- vtable slot 31.
 * Same-resource copy.  Vertical direction matters when src/dst rows
 * overlap; horizontal overlap is handled implicitly by memmove on each
 * row.
 *
 * Profiled: PROF[BlitRect] logs first call (one-shot) and a rolling
 * summary every 200 calls.  Useful for verifying the OverlappedBltBitMap
 * speedup in gfxbench2d.
 * ----------------------------------------------------------------------- */
static void chip_BlitRect(struct BoardInfo *bi, struct RenderInfoChip *ri,
                           WORD x, WORD y, WORD dx, WORD dy,
                           UWORD w, UWORD h, UBYTE mask)
{
    (void)bi; (void)mask;
    if (!ri || !ri->Memory || w == 0 || h == 0) return;
    if (g_chip_state) g_chip_state->stat_blitrect++;

    UWORD  bpp       = chip_format_bpp(ri->RGBFormat);
    ULONG  stride    = ri->BytesPerRow;
    UBYTE *src       = (UBYTE *)ri->Memory + (ULONG)y  * stride + (ULONG)x  * bpp;
    UBYTE *dst       = (UBYTE *)ri->Memory + (ULONG)dy * stride + (ULONG)dx * bpp;
    ULONG  row_bytes = (ULONG)w * bpp;

    struct ChipGPUState *gs = g_chip_state;
    struct ExecIFace *IExec = gs ? gs->IExec : NULL;

    CHIP_PROF_DEFINE(prof_blitrect, "BlitRect", 200);
    uint32 prof_t0 = chip_prof_begin(gs);

    /* Same-bitmap blit: rows of src and dst can overlap when dst lies
     * within the source rect (vertical) or per-row (horizontal).
     * Vertical: pick row direction so we don't clobber unread rows.
     * Horizontal-within-row: chip_lmemmove handles direction by
     * comparing pointers per row. */
    if (dy < y) {
        /* Forward: dst rows are above src rows, no row aliasing. */
        chip_copy_rect(IExec, src, stride, dst, stride, row_bytes, h);
    } else if (dy > y) {
        /* Reverse vertical: copy bottom-to-top so we read each src row
         * before its dst-row twin gets overwritten. */
        for (UWORD row = h; row > 0; row--) {
            const UBYTE *s = src + (ULONG)(row - 1) * stride;
            UBYTE *d       = dst + (ULONG)(row - 1) * stride;
            if ((((ULONG)s | (ULONG)d | row_bytes) & 3UL) == 0UL) {
                chip_lmemmove((const ULONG *)s, (ULONG *)d, row_bytes >> 2,
                              d < s);
            } else {
                if (d < s)
                    for (ULONG b = 0; b < row_bytes; b++) d[b] = s[b];
                else
                    for (ULONG b = row_bytes; b > 0; b--) d[b - 1] = s[b - 1];
            }
        }
    } else {
        /* Same row range (dy == y): horizontal-only overlap.  Per-row
         * chip_lmemmove handles forward/reverse based on pointer order. */
        for (UWORD row = 0; row < h; row++) {
            const UBYTE *s = src + (ULONG)row * stride;
            UBYTE *d       = dst + (ULONG)row * stride;
            if ((((ULONG)s | (ULONG)d | row_bytes) & 3UL) == 0UL) {
                chip_lmemmove((const ULONG *)s, (ULONG *)d, row_bytes >> 2,
                              d < s);
            } else if (d < s) {
                for (ULONG b = 0; b < row_bytes; b++) d[b] = s[b];
            } else {
                for (ULONG b = row_bytes; b > 0; b--) d[b - 1] = s[b - 1];
            }
        }
    }

    /* Do NOT flush dest rect here -- flushing only the dest leaves the old
     * position stale on the GPU, causing ghost trails.  Signal the flush
     * task instead so a single full-frame flush_all updates both old +
     * new positions atomically (no ghosts). */
    chip_flush_signal_activity(g_chip_state);

    /* Record copy throughput.  bytes = pixels copied (one direction). */
    chip_prof_end(gs, &prof_blitrect, prof_t0, (uint32)((ULONG)w * h * bpp));
}

/* -----------------------------------------------------------------------
 * chip_BlitRectNoMaskComplete -- vtable slot 39.
 *
 * P96 BlitRectNoMaskComplete opcode is the full BltBitMap "Minterm"
 * byte (graphics.library/BltBitMap autodoc).  Minterm encodes a
 * 3-input truth table over (A=mask, B=source, C=dest); the HIGH
 * nibble selects the behaviour when A=1 (inside the rectangle),
 * which is what we care about here ("NoMask" means A is implicitly 1).
 * Common values:
 *   0xC0 SrcCopy, 0x30 SrcInvert, 0x50 DstInvert, 0x60 SrcXorDst,
 *   0x00 Clear, 0xF0 Set.
 * Previously we did `opcode & 0xF` (low nibble) which collapses every
 * normal copy minterm to 0 -- so all gfxbench2d blits were drawn as
 * solid black (the visible "grey screen" symptom).  The right shift
 * is `opcode >> 4`.
 * ----------------------------------------------------------------------- */
static void chip_BlitRectNoMaskComplete(struct BoardInfo *bi,
                                         struct RenderInfoChip *src,
                                         struct RenderInfoChip *dst,
                                         WORD x, WORD y, WORD dx, WORD dy,
                                         UWORD w, UWORD h, UBYTE opcode,
                                         RGBFTYPE format)
{
    (void)bi;
    if (!src || !dst || !src->Memory || !dst->Memory || w == 0 || h == 0)
        return;
    if (g_chip_state) {
        g_chip_state->stat_blitrect_nmc++;
        g_chip_state->last_blit_src = src->Memory;
        g_chip_state->last_blit_dst = dst->Memory;
    }

    UWORD  bpp       = chip_format_bpp(format);
    ULONG  s_stride  = src->BytesPerRow;
    ULONG  d_stride  = dst->BytesPerRow;
    UBYTE *s_base    = (UBYTE *)src->Memory + (ULONG)y  * s_stride + (ULONG)x  * bpp;
    UBYTE *d_base    = (UBYTE *)dst->Memory + (ULONG)dy * d_stride + (ULONG)dx * bpp;
    ULONG  row_bytes = (ULONG)w * bpp;

    UBYTE rop = opcode >> 4;

    /* Per-rop profile buckets so the log shows where time is going.
     * gfxbench2d's BltBitMap-style tests route through this function
     * regardless of src==dst, so OverlappedBltBitMap shows up here
     * (PROF[NMC.SrcCopy]) not in chip_BlitRect. */
    CHIP_PROF_DEFINE(prof_nmc_srccopy,   "NMC.SrcCopy",   200);
    CHIP_PROF_DEFINE(prof_nmc_clear,     "NMC.Clear",     200);
    CHIP_PROF_DEFINE(prof_nmc_set,       "NMC.Set",       200);
    CHIP_PROF_DEFINE(prof_nmc_dstinvert, "NMC.DstInvert", 200);
    CHIP_PROF_DEFINE(prof_nmc_srcxor,    "NMC.SrcXorDst", 200);
    CHIP_PROF_DEFINE(prof_nmc_other,     "NMC.OtherRop",  200);
    uint32 prof_t0 = chip_prof_begin(g_chip_state);
    struct ChipProf *prof_target =
        (rop == 0xC) ? &prof_nmc_srccopy   :
        (rop == 0x0) ? &prof_nmc_clear     :
        (rop == 0xF) ? &prof_nmc_set       :
        (rop == 0x5) ? &prof_nmc_dstinvert :
        (rop == 0x6) ? &prof_nmc_srcxor    :
                       &prof_nmc_other;

    /* Vertical-direction reverse copy needed when src/dst alias and
     * dst rows are "below" src rows (dy > y).  For different bitmaps
     * or non-overlapping dy <= y, plain top->bottom memmove is correct. */
    BOOL reverse = (src->Memory == dst->Memory) &&
                   (dy > y || (dy == y && dx > x));

    struct ChipGPUState *gs = g_chip_state;
    struct ExecIFace *IExec = gs ? gs->IExec : NULL;

    if (rop == 0xC) {
        /* SrcCopy -- by far the most common case.  Different bitmaps OR
         * dy <= y in same-bitmap case: forward row order with CopyMemQuick
         * fast path.  reverse case: bottom-up, hand-rolled long copy. */
        if (!reverse) {
            chip_copy_rect(IExec, s_base, s_stride, d_base, d_stride,
                           row_bytes, h);
        } else {
            for (UWORD row = h; row > 0; row--) {
                const UBYTE *s = s_base + (ULONG)(row - 1) * s_stride;
                UBYTE *d       = d_base + (ULONG)(row - 1) * d_stride;
                if ((((ULONG)s | (ULONG)d | row_bytes) & 3UL) == 0UL) {
                    chip_lmemmove((const ULONG *)s, (ULONG *)d,
                                  row_bytes >> 2, d < s);
                } else if (d < s) {
                    for (ULONG b = 0; b < row_bytes; b++) d[b] = s[b];
                } else {
                    for (ULONG b = row_bytes; b > 0; b--) d[b - 1] = s[b - 1];
                }
            }
        }
    } else if (rop == 0x0) {
        chip_fill_rect_bytes(d_base, d_stride, row_bytes, h, 0x00);
    } else if (rop == 0xF) {
        chip_fill_rect_bytes(d_base, d_stride, row_bytes, h, 0xFF);
    } else if (rop == 0x5) {
        chip_xor_rect_with_const(d_base, d_stride, row_bytes, h);
    } else if (rop == 0x6) {
        chip_xor_rect_with_src(s_base, s_stride, d_base, d_stride, row_bytes, h);
    } else {
        /* Fallback: SrcCopy with overlap handling.  Log unknown ROPs
         * once per distinct value so we see what AOS4 actually asks for
         * if it diverges from the canonical 0xC/0x0/0xF/0x5/0x6 set. */
        {
            static volatile uint32 nmc_rop_warned;
            uint32 bit = 1U << (rop & 0x1F);
            if (!(nmc_rop_warned & bit)) {
                nmc_rop_warned |= bit;
                DCHIP("BlitRectNoMaskComplete: unknown rop=0x%x (opcode=0x%02x) "
                      "-- using SrcCopy fallback",
                      (unsigned)rop, (unsigned)opcode);
            }
        }
        if (!reverse) {
            chip_copy_rect(IExec, s_base, s_stride, d_base, d_stride,
                           row_bytes, h);
        } else {
            for (UWORD row = h; row > 0; row--) {
                const UBYTE *s = s_base + (ULONG)(row - 1) * s_stride;
                UBYTE *d       = d_base + (ULONG)(row - 1) * d_stride;
                if ((((ULONG)s | (ULONG)d | row_bytes) & 3UL) == 0UL) {
                    chip_lmemmove((const ULONG *)s, (ULONG *)d,
                                  row_bytes >> 2, d < s);
                } else if (d < s) {
                    for (ULONG b = 0; b < row_bytes; b++) d[b] = s[b];
                } else {
                    for (ULONG b = row_bytes; b > 0; b--) d[b - 1] = s[b - 1];
                }
            }
        }
    }

    chip_flush_signal_activity(g_chip_state);

    /* Record per-rop throughput.  bytes = pixels written. */
    chip_prof_end(g_chip_state, prof_target, prof_t0,
                  (uint32)((ULONG)w * h * bpp));
}

/* -----------------------------------------------------------------------
 * chip_InvertRect -- vtable slot 29.
 * ----------------------------------------------------------------------- */
static void chip_InvertRect(struct BoardInfo *bi, struct RenderInfoChip *ri,
                             WORD x, WORD y, UWORD w, UWORD h,
                             UBYTE mask, RGBFTYPE format)
{
    (void)bi; (void)mask;
    if (!ri || !ri->Memory || w == 0 || h == 0) return;
    if (g_chip_state) g_chip_state->stat_invertrect++;

    UWORD  bpp        = chip_format_bpp(format);
    ULONG  stride     = ri->BytesPerRow;
    UBYTE *base       = (UBYTE *)ri->Memory + (ULONG)y * stride + (ULONG)x * bpp;
    ULONG  row_bytes  = (ULONG)w * bpp;

    chip_xor_rect_with_const(base, stride, row_bytes, h);
    chip_flush(x, y, w, h);
}

/* -----------------------------------------------------------------------
 * chip_blit_fill_vtable -- wire all blit/draw callbacks into BoardInfo.
 * Called from chip_fill_boardinfo_vtable in chip_p96.c.
 * ----------------------------------------------------------------------- */
void chip_blit_fill_vtable(struct BoardInfo *bi)
{
    bi->BlitPlanar2Chunky        = chip_BlitPlanar2Chunky;
    bi->BlitPlanar2ChunkyFB      = chip_BlitPlanar2Chunky;
    bi->BlitPlanar2Direct        = chip_BlitPlanar2Direct;
    bi->BlitPlanar2DirectFB      = chip_BlitPlanar2Direct;
    bi->FillRect                 = chip_FillRect;
    bi->FillRectFB               = chip_FillRect;
    bi->InvertRect               = chip_InvertRect;
    bi->InvertRectFB             = chip_InvertRect;
    bi->BlitRect                 = chip_BlitRect;
    bi->BlitRectFB               = chip_BlitRect;
    bi->BlitTemplate             = chip_BlitTemplate;
    bi->BlitTemplateFB           = chip_BlitTemplate;
    bi->BlitPattern              = chip_BlitPattern;
    bi->BlitPatternFB            = chip_BlitPattern;
    bi->DrawLine                 = chip_DrawLine;
    bi->DrawLineFB               = chip_DrawLine;
    bi->BlitRectNoMaskComplete   = chip_BlitRectNoMaskComplete;
    bi->BlitRectNoMaskCompleteFB = chip_BlitRectNoMaskComplete;
}
