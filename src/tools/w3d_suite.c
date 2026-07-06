/*
 * w3d_suite.c -- self-checking Warp3D API regression suite (Phase 8b).
 *
 * Drives the stock Warp3D 53.27 FE over the W3D_VirtIOGPU backend exactly
 * the way real content does (InterleavedArray fmt COLOR|TCOORD_0, stride 40,
 * DrawElements -- the cow's path, which is also the texenv WIDE path) and
 * VERIFIES every feature by reading the rendered pixel back out of the
 * bitmap.  A regression is a named failing check with expected-vs-got
 * channel values, printed to stdout AND serial (DebugPrintF) -- never a
 * silently grey window.
 *
 * v1 coverage: clear, untextured gouraud, texenv REPLACE/MODULATE/DECAL/
 * BLEND, depth test on/off, alpha blending.  SetState rc-probes for
 * fog/alpha-test/cull/stencil are reported as INFO lines (8b telemetry --
 * they become real rendering checks as each feature lands).
 *
 * Renders into a private screen-sized board bitmap (friend = WB screen; the
 * FE sizes the context from W3D_CC_MODEID, so the bitmap must match), so the
 * suite never races Workbench for pixels.  Channel order in the readback is
 * CALIBRATED from three solid clears (red/green/blue), so the checks are
 * independent of the board's ARGB/BGRA layout.
 *
 * Build: -lauto executable.  Run from a shell; exit code = failed checks.
 */
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/dos.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>            /* LBM_* lock tags */
#include <warp3d/warp3d.h>
#include <interfaces/warp3d.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

static int RT_W = 0;   /* screen dims at runtime -- the FE sizes the ctx from MODEID */
static int RT_H = 0;
#define TEX_W 32
#define TEX_H 32
#define TOL  16          /* per-channel tolerance (format conversion slack) */

static struct Warp3DIFace *IW3D;
static W3D_Context        *g_ctx;
static struct BitMap      *g_bm;

static int g_checks = 0, g_fails = 0;

/* calibrated byte offsets of R,G,B inside a 32-bit pixel (from clears) */
static int off_r = -1, off_g = -1, off_b = -1;

/* one formatter for stdout + serial so both logs always agree */
static void NOTE(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("%s\n", line);
    IExec->DebugPrintF("[w3d_suite] %s\n", line);
}

/* ---- pixel readback ---------------------------------------------------- */

static BOOL read_pixel_raw(int x, int y, UBYTE out[4])
{
    struct TagItem lt[3];
    APTR base = NULL, lock;
    uint32 bpr = 0;

    lt[0].ti_Tag = LBM_BaseAddress; lt[0].ti_Data = (uint32)&base;
    lt[1].ti_Tag = LBM_BytesPerRow; lt[1].ti_Data = (uint32)&bpr;
    lt[2].ti_Tag = TAG_DONE;        lt[2].ti_Data = 0;
    lock = IGraphics->LockBitMapTagList(g_bm, lt);
    if (!lock || !base) return FALSE;
    {
        static int logged = 0;
        if (!logged) {
            logged = 1;
            NOTE("readback: bitmap base=%p bpr=%lu (compare with chip's "
                 "'converting ->' address in serial)", base, (unsigned long)bpr);
        }
    }
    memcpy(out, (UBYTE *)base + (uint32)y * bpr + (uint32)x * 4, 4);
    IGraphics->UnlockBitMap(lock);
    return TRUE;
}

static BOOL read_rgb(int x, int y, int *r, int *g, int *b)
{
    UBYTE px[4];
    if (!read_pixel_raw(x, y, px)) return FALSE;
    *r = px[off_r]; *g = px[off_g]; *b = px[off_b];
    return TRUE;
}

static void check_rgb(const char *name, int er, int eg, int eb)
{
    int r = -1, g = -1, b = -1;
    BOOL okread = read_rgb(RT_W / 2, RT_H / 2, &r, &g, &b);
    BOOL pass = okread &&
        (r >= er - TOL && r <= er + TOL) &&
        (g >= eg - TOL && g <= eg + TOL) &&
        (b >= eb - TOL && b <= eb + TOL);
    g_checks++;
    if (!pass) g_fails++;
    {
        char line[256];
        snprintf(line, sizeof(line), "%s %2d %s: got(%d,%d,%d) exp(%d,%d,%d)",
                 pass ? "ok  " : "FAIL", g_checks, name, r, g, b, er, eg, eb);
        printf("%s\n", line);
        IExec->DebugPrintF("[w3d_suite] %s\n", line);
    }
}

/* ---- draw helpers ------------------------------------------------------ */

/* Interleaved vertex: pos3 + colour4 + tc3 = 10 floats, stride 40 --
 * the exact layout the cow uses (fmt = W3D_VFORMAT_COLOR|W3D_VFORMAT_TCOORD_0). */
struct SVert { float x, y, z, r, g, b, a, u, v, w; };

static void set_vert(struct SVert *v, float x, float y, float z,
                     float r, float g, float b, float a, float u, float tv)
{
    v->x = x; v->y = y; v->z = z;
    v->r = r; v->g = g; v->b = b; v->a = a;
    v->u = u; v->v = tv; v->w = 0.0f;
}

/* Draw a screen-space quad (two triangles, indexed) covering x0..x1/y0..y1
 * at depth z with one flat colour + full 0..1 texcoords. */
static uint32 draw_quad(float x0, float y0, float x1, float y1, float z,
                        float r, float g, float b, float a)
{
    static struct SVert verts[4];
    static uint16 idx[6] = { 0, 1, 2, 0, 2, 3 };
    uint32 rc;

    set_vert(&verts[0], x0, y0, z, r, g, b, a, 0.0f, 0.0f);
    set_vert(&verts[1], x1, y0, z, r, g, b, a, 1.0f, 0.0f);
    set_vert(&verts[2], x1, y1, z, r, g, b, a, 1.0f, 1.0f);
    set_vert(&verts[3], x0, y1, z, r, g, b, a, 0.0f, 1.0f);

    rc = IW3D->W3D_InterleavedArray(g_ctx, verts, sizeof(struct SVert),
                                    W3D_VFORMAT_COLOR | W3D_VFORMAT_TCOORD_0, 0);
    if (rc != W3D_SUCCESS) return rc;
    return IW3D->W3D_DrawElements(g_ctx, W3D_PRIMITIVE_TRIANGLES,
                                  W3D_INDEX_UWORD, 6, idx);
}

static void present(void)
{
    IW3D->W3D_FlushFrame(g_ctx);
    IW3D->W3D_WaitIdle(g_ctx);
}

/* ---- textures ---------------------------------------------------------- */

/* Solid-colour RGBA texture (32bpp raw, the backend's proven upload format).
 * Solid colour also makes the linear-filtered sample exact at every pixel. */
static uint32 tex_pixels[TEX_W * TEX_H];

static W3D_Texture *make_tex(UBYTE r, UBYTE g, UBYTE b, UBYTE a)
{
    uint32 err = 0;
    int i;
    W3D_Texture *tex;
    /* backend uploads as R8G8B8A8 byte order */
    for (i = 0; i < TEX_W * TEX_H; i++) {
        UBYTE *p = (UBYTE *)&tex_pixels[i];
        p[0] = r; p[1] = g; p[2] = b; p[3] = a;
    }
    tex = IW3D->W3D_AllocTexObjTags(g_ctx, &err,
                                    W3D_ATO_IMAGE,  tex_pixels,
                                    W3D_ATO_FORMAT, W3D_R8G8B8A8,
                                    W3D_ATO_WIDTH,  TEX_W,
                                    W3D_ATO_HEIGHT, TEX_H,
                                    TAG_DONE);
    if (!tex)
        NOTE("AllocTexObjTags FAILED err=%lu", (unsigned long)err);
    return tex;
}

/* Left half solid red, right half solid blue -- the u=0.5 boundary lands on
 * the screen centre, so NEAREST reads a pure texel while LINEAR mixes:
 * the one texture that makes the filter mode visible to a single readback. */
static W3D_Texture *make_tex_halves(void)
{
    uint32 err = 0;
    int x, y;
    W3D_Texture *tex;
    for (y = 0; y < TEX_H; y++)
        for (x = 0; x < TEX_W; x++) {
            UBYTE *p = (UBYTE *)&tex_pixels[y * TEX_W + x];
            p[0] = (x < TEX_W / 2) ? 255 : 0;  /* r */
            p[1] = 0;                          /* g */
            p[2] = (x < TEX_W / 2) ? 0 : 255;  /* b */
            p[3] = 255;                        /* a */
        }
    tex = IW3D->W3D_AllocTexObjTags(g_ctx, &err,
                                    W3D_ATO_IMAGE,  tex_pixels,
                                    W3D_ATO_FORMAT, W3D_R8G8B8A8,
                                    W3D_ATO_WIDTH,  TEX_W,
                                    W3D_ATO_HEIGHT, TEX_H,
                                    TAG_DONE);
    if (!tex)
        NOTE("AllocTexObjTags(halves) FAILED err=%lu", (unsigned long)err);
    return tex;
}

/* Filter check: at the red/blue boundary a NEAREST sample is pure (one
 * channel saturated) while a LINEAR sample is a mix (both mid-range). */
static void check_filter(const char *name, BOOL expect_mixed)
{
    int r = -1, g = -1, b = -1;
    BOOL okread = read_rgb(RT_W / 2, RT_H / 2, &r, &g, &b);
    BOOL mixed = okread && (r > 40 && r < 215 && b > 40 && b < 215);
    BOOL pure  = okread && ((r > 215 && b < 40) || (r < 40 && b > 215));
    BOOL pass  = expect_mixed ? mixed : pure;
    g_checks++;
    if (!pass) g_fails++;
    {
        char line[256];
        snprintf(line, sizeof(line), "%s %2d %s: got(%d,%d,%d) expected %s",
                 pass ? "ok  " : "FAIL", g_checks, name, r, g, b,
                 expect_mixed ? "mixed r+b" : "pure r or b");
        printf("%s\n", line);
        IExec->DebugPrintF("[w3d_suite] %s\n", line);
    }
}

/* ---- calibration ------------------------------------------------------- */

static uint32 draw_quad(float x0, float y0, float x1, float y1, float z,
                        float r, float g, float b, float a);

static BOOL calibrate_channels(void)
{
    /* Three solid full-screen DRAWS (the cow-proven path -- deliberately not
     * clear-only, which is checked separately); each colour channel is the
     * byte that goes high in exactly its own frame -- immune to whatever the
     * present conversion does with alpha. */
    UBYTE px_r[4], px_g[4], px_b[4];
    int i;

    draw_quad(0, 0, (float)RT_W, (float)RT_H, 0.5f, 1.0f, 0.0f, 0.0f, 1.0f);
    present();
    if (!read_pixel_raw(RT_W / 2, RT_H / 2, px_r)) return FALSE;

    draw_quad(0, 0, (float)RT_W, (float)RT_H, 0.5f, 0.0f, 1.0f, 0.0f, 1.0f);
    present();
    if (!read_pixel_raw(RT_W / 2, RT_H / 2, px_g)) return FALSE;

    draw_quad(0, 0, (float)RT_W, (float)RT_H, 0.5f, 0.0f, 0.0f, 1.0f, 1.0f);
    present();
    if (!read_pixel_raw(RT_W / 2, RT_H / 2, px_b)) return FALSE;

    for (i = 0; i < 4; i++) {
        BOOL hr = px_r[i] > 200, hg = px_g[i] > 200, hb = px_b[i] > 200;
        if      ( hr && !hg && !hb) off_r = i;
        else if (!hr &&  hg && !hb) off_g = i;
        else if (!hr && !hg &&  hb) off_b = i;
    }
    NOTE("channel layout: R@%d G@%d B@%d (raw red clear %02x %02x %02x %02x)",
         off_r, off_g, off_b, px_r[0], px_r[1], px_r[2], px_r[3]);
    return (off_r >= 0 && off_g >= 0 && off_b >= 0);
}

/* ---- main -------------------------------------------------------------- */

int main(void)
{
    struct Library     *W3DBase = NULL;
    struct Screen      *scr = NULL;
    W3D_Texture        *tex_blue = NULL, *tex_white = NULL, *tex_halves = NULL;
    W3D_Color           envcol;
    uint32              err = 0, rc;

    scr = IIntuition->LockPubScreen(NULL);
    if (!scr) { printf("w3d_suite: no public screen\n"); return 20; }
    RT_W = scr->Width;
    RT_H = scr->Height;

    /* The cow's proven recipe: screen-depth board bitmap, MINPLANES. */
    g_bm = IGraphics->AllocBitMap(RT_W, RT_H,
                                  IGraphics->GetBitMapAttr(scr->RastPort.BitMap,
                                                           BMA_DEPTH),
                                  BMF_DISPLAYABLE | BMF_MINPLANES,
                                  scr->RastPort.BitMap);
    if (!g_bm) {
        printf("w3d_suite: AllocBitMap failed\n");
        IIntuition->UnlockPubScreen(NULL, scr);
        return 20;
    }

    W3DBase = IExec->OpenLibrary("Warp3D.library", 0);
    IW3D = W3DBase ? (struct Warp3DIFace *)
                     IExec->GetInterface(W3DBase, "main", 1, NULL) : NULL;
    if (!IW3D) {
        printf("w3d_suite: no Warp3D main interface\n");
        goto out;
    }

    {
        /* MODEID is mandatory when the bitmap is not a screen's (cow recipe) */
        struct TagItem cctags[5];
        cctags[0].ti_Tag = W3D_CC_MODEID;     cctags[0].ti_Data =
            IGraphics->GetVPModeID(&scr->ViewPort);
        cctags[1].ti_Tag = W3D_CC_BITMAP;     cctags[1].ti_Data = (uint32)g_bm;
        cctags[2].ti_Tag = W3D_CC_YOFFSET;    cctags[2].ti_Data = 0;
        cctags[3].ti_Tag = W3D_CC_DRIVERTYPE; cctags[3].ti_Data = W3D_DRIVER_BEST;
        cctags[4].ti_Tag = TAG_DONE;          cctags[4].ti_Data = 0;
        g_ctx = IW3D->W3D_CreateContext(&err, cctags);
    }
    if (!g_ctx) {
        printf("w3d_suite: CreateContext failed err=%lu\n", (unsigned long)err);
        goto out;
    }
    NOTE("context %lux%lu on private bitmap", (unsigned long)g_ctx->width,
         (unsigned long)g_ctx->height);

    {
        W3D_Scissor sc;
        sc.left = 0; sc.top = 0; sc.width = RT_W; sc.height = RT_H;
        IW3D->W3D_SetDrawRegion(g_ctx, g_bm, 0, &sc);
    }

    IW3D->W3D_SetState(g_ctx, W3D_GOURAUD,    W3D_ENABLE);
    IW3D->W3D_SetState(g_ctx, W3D_TEXMAPPING, W3D_ENABLE);
    IW3D->W3D_SetState(g_ctx, W3D_ZBUFFER,        W3D_DISABLE);
    IW3D->W3D_SetState(g_ctx, W3D_ZBUFFERUPDATE,  W3D_DISABLE);
    IW3D->W3D_SetState(g_ctx, W3D_BLENDING,       W3D_DISABLE);

    if (!calibrate_channels()) {
        printf("w3d_suite: channel calibration FAILED (no readback?)\n");
        g_fails++; goto summary;
    }

    /* 1: clear colour survives present + readback (green, distinct from
     *    both calibration clears) */
    IW3D->W3D_ClearDrawRegion(g_ctx, 0xFF00FF00);
    present();
    check_rgb("clear green", 0, 255, 0);

    /* 2: untextured gouraud quad (narrow path, colour from the array) --
     *    runs BEFORE any texture is ever bound */
    IW3D->W3D_ClearDrawRegion(g_ctx, 0xFF000000);
    rc = draw_quad(0, 0, (float)RT_W, (float)RT_H, 0.5f, 1.0f, 0.5f, 0.0f, 1.0f);
    if (rc != W3D_SUCCESS)
        NOTE("draw_quad rc=%lu", (unsigned long)rc);
    present();
    check_rgb("gouraud untextured", 255, 127, 0);

    /* textures for the texenv block */
    tex_blue  = make_tex(0, 0, 255, 255);
    tex_white = make_tex(255, 255, 255, 255);
    if (!tex_blue || !tex_white) { g_fails++; goto summary; }

    /* 3: REPLACE -- pure texel regardless of vertex colour (narrow textured) */
    IW3D->W3D_BindTexture(g_ctx, 0, tex_blue);
    IW3D->W3D_SetTexEnv(g_ctx, tex_blue, W3D_REPLACE, NULL);
    IW3D->W3D_ClearDrawRegion(g_ctx, 0xFF000000);
    draw_quad(0, 0, (float)RT_W, (float)RT_H, 0.5f, 1.0f, 0.0f, 0.0f, 1.0f);  /* red verts */
    present();
    check_rgb("texenv REPLACE (blue tex, red verts)", 0, 0, 255);

    /* 4: MODULATE -- texel * vertex colour (WIDE path).  Blue tex * 50%
     *    grey verts = half blue. */
    IW3D->W3D_SetTexEnv(g_ctx, tex_blue, W3D_MODULATE, NULL);
    IW3D->W3D_ClearDrawRegion(g_ctx, 0xFF000000);
    draw_quad(0, 0, (float)RT_W, (float)RT_H, 0.5f, 0.5f, 0.5f, 0.5f, 1.0f);
    present();
    check_rgb("texenv MODULATE (blue tex, grey verts)", 0, 0, 127);

    /* 5: DECAL -- lerp(fragment, texel, texel.a); opaque texel wins
     *    completely.  Red verts would show through only on a=0 texels
     *    (MODULATE here would give black -- a real discriminator). */
    IW3D->W3D_SetTexEnv(g_ctx, tex_blue, W3D_DECAL, NULL);
    IW3D->W3D_ClearDrawRegion(g_ctx, 0xFF000000);
    draw_quad(0, 0, (float)RT_W, (float)RT_H, 0.5f, 1.0f, 0.0f, 0.0f, 1.0f);
    present();
    check_rgb("texenv DECAL (opaque blue tex, red verts)", 0, 0, 255);

    /* 6: BLEND -- C = Cf*(1-Ct) + Cenv*Ct; white texel selects the env
     *    colour completely.  Env = magenta. */
    envcol.r = 1.0f; envcol.g = 0.0f; envcol.b = 1.0f; envcol.a = 1.0f;
    IW3D->W3D_BindTexture(g_ctx, 0, tex_white);
    IW3D->W3D_SetTexEnv(g_ctx, tex_white, W3D_BLEND, &envcol);
    IW3D->W3D_ClearDrawRegion(g_ctx, 0xFF000000);
    draw_quad(0, 0, (float)RT_W, (float)RT_H, 0.5f, 1.0f, 0.0f, 0.0f, 1.0f);
    present();
    check_rgb("texenv BLEND (white tex, magenta env)", 255, 0, 255);

    /* back to REPLACE + untextured for the depth/blend block */
    IW3D->W3D_SetTexEnv(g_ctx, tex_white, W3D_REPLACE, NULL);
    rc = IW3D->W3D_BindTexture(g_ctx, 0, NULL);
    NOTE("BindTexture(NULL) rc=%lu (0=unbound ok)", (unsigned long)rc);

    /* 7: depth test on -- near green quad first, far red quad second;
     *    red must lose against the depth buffer. */
    IW3D->W3D_SetState(g_ctx, W3D_ZBUFFER,       W3D_ENABLE);
    IW3D->W3D_SetState(g_ctx, W3D_ZBUFFERUPDATE, W3D_ENABLE);
    IW3D->W3D_ClearDrawRegion(g_ctx, 0xFF000000);   /* also clears Z to 1.0 */
    draw_quad(0, 0, (float)RT_W, (float)RT_H, 0.2f, 0.0f, 1.0f, 0.0f, 1.0f);  /* near green */
    draw_quad(0, 0, (float)RT_W, (float)RT_H, 0.8f, 1.0f, 0.0f, 0.0f, 1.0f);  /* far red */
    present();
    check_rgb("depth test occludes far quad", 0, 255, 0);

    /* 8: depth test off -- same order, last draw wins */
    IW3D->W3D_SetState(g_ctx, W3D_ZBUFFER,       W3D_DISABLE);
    IW3D->W3D_SetState(g_ctx, W3D_ZBUFFERUPDATE, W3D_DISABLE);
    IW3D->W3D_ClearDrawRegion(g_ctx, 0xFF000000);
    draw_quad(0, 0, (float)RT_W, (float)RT_H, 0.2f, 0.0f, 1.0f, 0.0f, 1.0f);
    draw_quad(0, 0, (float)RT_W, (float)RT_H, 0.8f, 1.0f, 0.0f, 0.0f, 1.0f);
    present();
    check_rgb("depth off: last draw wins", 255, 0, 0);

    /* 9: alpha blending -- white quad at a=0.5, SRC_ALPHA/ONE_MINUS_SRC_ALPHA
     *    over a black clear = mid grey */
    IW3D->W3D_SetBlendMode(g_ctx, W3D_SRC_ALPHA, W3D_ONE_MINUS_SRC_ALPHA);
    IW3D->W3D_SetState(g_ctx, W3D_BLENDING, W3D_ENABLE);
    IW3D->W3D_ClearDrawRegion(g_ctx, 0xFF000000);
    draw_quad(0, 0, (float)RT_W, (float)RT_H, 0.5f, 1.0f, 1.0f, 1.0f, 0.5f);
    present();
    check_rgb("alpha blend 50% white over black", 127, 127, 127);
    IW3D->W3D_SetState(g_ctx, W3D_BLENDING, W3D_DISABLE);

    /* --- 8b batch 1: alpha test, colour mask, cull, lines, filter --- */

    /* 10/11: alpha test GREATER 0.5 -- a=0.25 quad discarded, a=0.75 drawn */
    {
        W3D_Float aref = 0.5f;
        rc = IW3D->W3D_SetAlphaMode(g_ctx, W3D_A_GREATER, &aref);
        NOTE("SetAlphaMode(GREATER,0.5) rc=%lu", (unsigned long)rc);
    }
    IW3D->W3D_SetState(g_ctx, W3D_ALPHATEST, W3D_ENABLE);
    IW3D->W3D_ClearDrawRegion(g_ctx, 0xFF000000);
    draw_quad(0, 0, (float)RT_W, (float)RT_H, 0.5f, 1.0f, 0.0f, 0.0f, 0.25f);
    present();
    check_rgb("alpha test discards a=0.25", 0, 0, 0);
    IW3D->W3D_ClearDrawRegion(g_ctx, 0xFF000000);
    draw_quad(0, 0, (float)RT_W, (float)RT_H, 0.5f, 0.0f, 1.0f, 0.0f, 0.75f);
    present();
    check_rgb("alpha test passes a=0.75", 0, 255, 0);
    IW3D->W3D_SetState(g_ctx, W3D_ALPHATEST, W3D_DISABLE);

    /* 12: colour mask -- red channel write disabled, white draw = cyan */
    rc = IW3D->W3D_SetColorMask(g_ctx, FALSE, TRUE, TRUE, TRUE);
    NOTE("SetColorMask(0,1,1,1) rc=%lu", (unsigned long)rc);
    IW3D->W3D_ClearDrawRegion(g_ctx, 0xFF000000);
    draw_quad(0, 0, (float)RT_W, (float)RT_H, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f);
    present();
    check_rgb("colour mask blocks red channel", 0, 255, 255);
    IW3D->W3D_SetColorMask(g_ctx, TRUE, TRUE, TRUE, TRUE);

    /* 13/14: backface cull -- our quads are CCW in NDC; with front=CCW the
     * quad survives a back-cull, with front=CW it IS the back face */
    IW3D->W3D_SetFrontFace(g_ctx, W3D_CCW);
    IW3D->W3D_SetState(g_ctx, W3D_CULLFACE, W3D_ENABLE);
    IW3D->W3D_ClearDrawRegion(g_ctx, 0xFF000000);
    draw_quad(0, 0, (float)RT_W, (float)RT_H, 0.5f, 0.0f, 1.0f, 0.0f, 1.0f);
    present();
    check_rgb("cull back keeps CCW front face", 0, 255, 0);
    IW3D->W3D_SetFrontFace(g_ctx, W3D_CW);
    IW3D->W3D_ClearDrawRegion(g_ctx, 0xFF000000);
    draw_quad(0, 0, (float)RT_W, (float)RT_H, 0.5f, 0.0f, 1.0f, 0.0f, 1.0f);
    present();
    check_rgb("cull removes CW-front (now back) face", 0, 0, 0);
    IW3D->W3D_SetState(g_ctx, W3D_CULLFACE, W3D_DISABLE);
    IW3D->W3D_SetFrontFace(g_ctx, W3D_CCW);

    /* 15: LINES primitive -- white horizontal line through the centre row */
    {
        static struct SVert lverts[2];
        static uint16 lidx[2] = { 0, 1 };
        set_vert(&lverts[0], 0.0f,        (float)(RT_H / 2), 0.5f,
                 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f);
        set_vert(&lverts[1], (float)RT_W, (float)(RT_H / 2), 0.5f,
                 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f);
        IW3D->W3D_ClearDrawRegion(g_ctx, 0xFF000000);
        IW3D->W3D_InterleavedArray(g_ctx, lverts, sizeof(struct SVert),
                                   W3D_VFORMAT_COLOR | W3D_VFORMAT_TCOORD_0, 0);
        rc = IW3D->W3D_DrawElements(g_ctx, W3D_PRIMITIVE_LINES,
                                    W3D_INDEX_UWORD, 2, lidx);
        NOTE("DrawElements(LINES) rc=%lu", (unsigned long)rc);
        present();
        check_rgb("LINES primitive draws centre row", 255, 255, 255);
    }

    /* 16/17: texture filter -- boundary sample pure under NEAREST, mixed
     * under LINEAR (half-red/half-blue texture, REPLACE) */
    tex_halves = make_tex_halves();
    if (tex_halves) {
        IW3D->W3D_BindTexture(g_ctx, 0, tex_halves);
        IW3D->W3D_SetTexEnv(g_ctx, tex_halves, W3D_REPLACE, NULL);
        rc = IW3D->W3D_SetFilter(g_ctx, tex_halves, W3D_NEAREST, W3D_NEAREST);
        NOTE("SetFilter(NEAREST) rc=%lu", (unsigned long)rc);
        IW3D->W3D_ClearDrawRegion(g_ctx, 0xFF000000);
        draw_quad(0, 0, (float)RT_W, (float)RT_H, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f);
        present();
        check_filter("filter NEAREST pure at texel boundary", FALSE);
        rc = IW3D->W3D_SetFilter(g_ctx, tex_halves, W3D_LINEAR, W3D_LINEAR);
        NOTE("SetFilter(LINEAR) rc=%lu", (unsigned long)rc);
        IW3D->W3D_ClearDrawRegion(g_ctx, 0xFF000000);
        draw_quad(0, 0, (float)RT_W, (float)RT_H, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f);
        present();
        check_filter("filter LINEAR mixes at texel boundary", TRUE);
        IW3D->W3D_BindTexture(g_ctx, 0, NULL);
    }

    /* 18-23: fog -- LINEAR at three depths (untextured = CPU premix),
     * textured REPLACE+fog (wide repfog FS), EXP, and fog-off identity */
    /* classic call order: the FOGGING state must be enabled BEFORE
     * SetFogParams or the stock FE refuses with W3D_ILLEGALINPUT */
    IW3D->W3D_SetState(g_ctx, W3D_FOGGING, W3D_ENABLE);
    {
        W3D_Fog fog;
        fog.fog_start = 1.0f; fog.fog_end = 0.0f; fog.fog_density = 1.0f;  /* W3D: start > end */
        fog.fog_color.r = 1.0f; fog.fog_color.g = 0.0f; fog.fog_color.b = 0.0f;
        rc = IW3D->W3D_SetFogParams(g_ctx, &fog, W3D_FOG_LINEAR);
        NOTE("SetFogParams(LINEAR red 0..1) rc=%lu", (unsigned long)rc);
    }
    IW3D->W3D_ClearDrawRegion(g_ctx, 0xFF000000);
    draw_quad(0, 0, (float)RT_W, (float)RT_H, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f);
    present();
    check_rgb("fog LINEAR z=0: unfogged green", 0, 255, 0);
    IW3D->W3D_ClearDrawRegion(g_ctx, 0xFF000000);
    draw_quad(0, 0, (float)RT_W, (float)RT_H, 0.999f, 0.0f, 1.0f, 0.0f, 1.0f);
    present();
    check_rgb("fog LINEAR z=1: full fog red", 255, 0, 0);
    IW3D->W3D_ClearDrawRegion(g_ctx, 0xFF000000);
    draw_quad(0, 0, (float)RT_W, (float)RT_H, 0.5f, 0.0f, 1.0f, 0.0f, 1.0f);
    present();
    check_rgb("fog LINEAR z=0.5: half fog", 127, 127, 0);
    if (tex_blue) {
        IW3D->W3D_BindTexture(g_ctx, 0, tex_blue);
        IW3D->W3D_SetTexEnv(g_ctx, tex_blue, W3D_REPLACE, NULL);
        IW3D->W3D_ClearDrawRegion(g_ctx, 0xFF000000);
        draw_quad(0, 0, (float)RT_W, (float)RT_H, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f);
        present();
        check_rgb("fog on REPLACE texture (wide repfog FS)", 127, 0, 127);
        IW3D->W3D_BindTexture(g_ctx, 0, NULL);
    }
    {
        W3D_Fog fog;
        fog.fog_start = 1.0f; fog.fog_end = 0.0f; fog.fog_density = 1.0f;  /* W3D: start > end */
        fog.fog_color.r = 1.0f; fog.fog_color.g = 0.0f; fog.fog_color.b = 0.0f;
        IW3D->W3D_SetFogParams(g_ctx, &fog, W3D_FOG_EXP);
    }
    /* z chosen where EXP and LINEAR differ by ~90/channel -- if the mode
     * never reached the driver this check catches it (LINEAR would give
     * (242,13,0)) */
    IW3D->W3D_ClearDrawRegion(g_ctx, 0xFF000000);
    draw_quad(0, 0, (float)RT_W, (float)RT_H, 0.95f, 0.0f, 1.0f, 0.0f, 1.0f);
    present();
    check_rgb("fog EXP z=0.95 d=1: f=0.39", 156, 99, 0);
    IW3D->W3D_SetState(g_ctx, W3D_FOGGING, W3D_DISABLE);
    IW3D->W3D_ClearDrawRegion(g_ctx, 0xFF000000);
    draw_quad(0, 0, (float)RT_W, (float)RT_H, 0.9f, 0.0f, 1.0f, 0.0f, 1.0f);
    present();
    check_rgb("fog off: identity restored", 0, 255, 0);

    /* --- 8b telemetry: what does the FE claim for the not-yet-done set? --- */
    NOTE("INFO SetState rc: FOG=%lu ALPHATEST=%lu CULL=%lu STENCIL=%lu (0=accepted)",
         (unsigned long)IW3D->W3D_SetState(g_ctx, W3D_FOGGING,       W3D_ENABLE),
         (unsigned long)IW3D->W3D_SetState(g_ctx, W3D_ALPHATEST,     W3D_ENABLE),
         (unsigned long)IW3D->W3D_SetState(g_ctx, W3D_CULLFACE,      W3D_ENABLE),
         (unsigned long)IW3D->W3D_SetState(g_ctx, W3D_STENCILBUFFER, W3D_ENABLE));
    IW3D->W3D_SetState(g_ctx, W3D_FOGGING,       W3D_DISABLE);
    IW3D->W3D_SetState(g_ctx, W3D_ALPHATEST,     W3D_DISABLE);
    IW3D->W3D_SetState(g_ctx, W3D_CULLFACE,      W3D_DISABLE);
    IW3D->W3D_SetState(g_ctx, W3D_STENCILBUFFER, W3D_DISABLE);

summary:
    {
        char line[128];
        snprintf(line, sizeof(line), "w3d_suite: %d checks, %d failed -> %s",
                 g_checks, g_fails, g_fails ? "FAIL" : "ALL PASS");
        printf("%s\n", line);
        IExec->DebugPrintF("[w3d_suite] %s\n", line);
    }

out:
    if (tex_blue)   IW3D->W3D_FreeTexObj(g_ctx, tex_blue);
    if (tex_white)  IW3D->W3D_FreeTexObj(g_ctx, tex_white);
    if (tex_halves) IW3D->W3D_FreeTexObj(g_ctx, tex_halves);
    if (g_ctx)     IW3D->W3D_DestroyContext(g_ctx);
    if (IW3D)      IExec->DropInterface((struct Interface *)IW3D);
    if (W3DBase)   IExec->CloseLibrary(W3DBase);
    if (g_bm)      IGraphics->FreeBitMap(g_bm);
    if (scr)       IIntuition->UnlockPubScreen(NULL, scr);
    return g_fails;
}
