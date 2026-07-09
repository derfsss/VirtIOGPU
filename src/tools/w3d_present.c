/*
 * w3d_present.c -- minimal windowed-present reproducer (2026-07-09 soak).
 *
 * The soak found that MiniGL apps (Quake2, gears68k) render a GREY window
 * while the direct-W3D cow renders fine.  Working theory: the chip syncs
 * GPU-rendered content into the bitmap only on LockBitMap (the
 * "converting ->" path), and MiniGL presents by blitting the drawregion
 * bitmap WITHOUT locking it first -- so the blit copies a bitmap the GPU
 * result never reached.
 *
 * This tool is the smallest possible native recreation of that present
 * pattern -- pure Warp3D 53.27 FE API, no MiniGL, no 68k:
 *
 *   window on WB  +  offscreen friend bitmap ("drawregion")  +
 *   rotating gouraud triangle  +  FlushFrame/WaitIdle  +
 *   BltBitMapRastPort(bitmap -> window)          <- MiniGL-style, DEFAULT
 *
 * Run modes:
 *   w3d_present            InterleavedArray + W3D_DrawArray -- the MiniGL
 *                          call pattern (expected: REPRO, grey window:
 *                          backend DrawArray ignores the interleaved
 *                          array and bails ILLEGALINPUT with vtx_ptr
 *                          NULL, and the deferred clear never executes)
 *   w3d_present ELEMENTS   InterleavedArray + W3D_DrawElements -- the
 *                          suite/cow path (control: triangle visible;
 *                          VERIFIED rendering + blit-present on
 *                          2026-07-09, so present-by-blit itself is fine)
 *   w3d_present LOCK       additionally LockBitMap+Unlock before each
 *                          blit (2026-07-09: made no difference --
 *                          the original lock-sync theory is falsified)
 *   w3d_present [mode] n   run n frames (default 300)
 *
 * SELF-CHECKING: every 60 frames it reads the pixel at the window centre
 * off the SCREEN (cybergraphics ReadRGBPixel on the window rastport) and
 * prints it; the offscreen bitmap is prefilled 0x404040 grey, each frame
 * clears to dark blue 0x000060, and the triangle is red/green/blue
 * gouraud -- so:
 *     centre stays ~0x404040       -> present broken (rc 5)
 *     centre is blue or triangle   -> present works  (rc 0)
 *
 * Build: -lauto executable (make build/w3d_present).  Copy to RAM: with
 * `protect rwed` like the other tools.
 */
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/dos.h>
#include <proto/cybergraphics.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <warp3d/warp3d.h>
#include <interfaces/warp3d.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#define VW 640
#define VH 480
#define PREFILL 0xFF404040u   /* "never synced" sentinel in the bitmap */
#define CLEARCOL 0xFF000060u  /* per-frame W3D clear (dark blue) */

static struct Warp3DIFace   *IW3D;
struct CyberGfxIFace        *ICyberGfx;   /* proto header declares it extern */
static W3D_Context          *ctx;
static struct BitMap        *bm;
static struct Window        *win;

static void NOTE(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("%s\n", line);
    IExec->DebugPrintF("[w3d_present] %s\n", line);
}

/* interleaved vertex, the suite/cow-proven layout: pos3+col4+tc3, stride 40 */
struct SVert { float x, y, z, r, g, b, a, u, v, w; };

static BOOL use_elements = FALSE;   /* FALSE = MiniGL-style DrawArray */

static uint32 draw_triangle(float cx, float cy, float ux, float uy, float rad)
{
    /* three base directions 120 degrees apart, rotated by (ux,uy) */
    static const float bx[3] = { 0.0f,  0.866f, -0.866f };
    static const float by[3] = { -1.0f, 0.5f,    0.5f   };
    static const float cr[3] = { 1.0f, 0.0f, 0.0f };
    static const float cg[3] = { 0.0f, 1.0f, 0.0f };
    static const float cb[3] = { 0.0f, 0.0f, 1.0f };
    static struct SVert v[3];
    static uint16 idx[3] = { 0, 1, 2 };
    uint32 rc;
    int i;

    for (i = 0; i < 3; i++) {
        /* rotate base dir by the running unit vector (2D rotation) */
        float rx = bx[i] * ux - by[i] * uy;
        float ry = bx[i] * uy + by[i] * ux;
        v[i].x = cx + rad * rx;
        v[i].y = cy + rad * ry;
        v[i].z = 0.5f;
        v[i].r = cr[i]; v[i].g = cg[i]; v[i].b = cb[i]; v[i].a = 1.0f;
        v[i].u = 0.0f;  v[i].v = 0.0f;  v[i].w = 0.0f;
    }
    rc = IW3D->W3D_InterleavedArray(ctx, v, sizeof(struct SVert),
                                    W3D_VFORMAT_COLOR | W3D_VFORMAT_TCOORD_0, 0);
    if (rc != W3D_SUCCESS) return rc;
    if (use_elements)
        return IW3D->W3D_DrawElements(ctx, W3D_PRIMITIVE_TRIANGLES,
                                      W3D_INDEX_UWORD, 3, idx);
    /* the MiniGL pattern: draw the bound array directly, no index list */
    return IW3D->W3D_DrawArray(ctx, W3D_PRIMITIVE_TRIANGLES, 0, 3);
}

/* the theory's crux: force the chip's GPU->bitmap sync before the blit */
static void sync_bitmap(void)
{
    struct TagItem lt[1];
    APTR lock;
    lt[0].ti_Tag = TAG_DONE; lt[0].ti_Data = 0;
    lock = IGraphics->LockBitMapTagList(bm, lt);
    if (lock) IGraphics->UnlockBitMap(lock);
}

int main(int argc, char **argv)
{
    struct Library *W3DBase = NULL, *CGXBase = NULL;
    struct Screen  *scr = NULL;
    BOOL   do_lock = FALSE;
    int    frames = 300, frame, rc = 20;
    uint32 err = 0;
    float  ux = 1.0f, uy = 0.0f;            /* rotating unit vector */
    const float RC_ = 0.998630f, RS_ = 0.052336f;   /* 3 deg/frame */
    uint32 centre = 0;
    int    i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "LOCK") || !strcmp(argv[i], "lock"))
            do_lock = TRUE;
        else if (!strcmp(argv[i], "ELEMENTS") || !strcmp(argv[i], "elements"))
            use_elements = TRUE;
        else if (atoi(argv[i]) > 0)
            frames = atoi(argv[i]);
    }

    scr = IIntuition->LockPubScreen(NULL);
    if (!scr) { NOTE("no public screen"); return 20; }

    win = IIntuition->OpenWindowTags(NULL,
        WA_Title,        do_lock ? (uint32)"w3d_present LOCK (control)"
                                 : (uint32)"w3d_present NO-LOCK (MiniGL-style)",
        WA_InnerWidth,   VW,
        WA_InnerHeight,  VH,
        WA_Left,         80,
        WA_Top,          60,
        WA_DragBar,      TRUE,
        WA_CloseGadget,  TRUE,
        WA_DepthGadget,  TRUE,
        WA_Activate,     TRUE,
        WA_IDCMP,        IDCMP_CLOSEWINDOW,
        WA_PubScreen,    (uint32)scr,
        TAG_DONE);
    if (!win) { NOTE("OpenWindow failed"); goto out; }

    /* the MiniGL-style offscreen drawregion: friend bitmap, screen depth */
    bm = IGraphics->AllocBitMap(VW, VH,
             IGraphics->GetBitMapAttr(scr->RastPort.BitMap, BMA_DEPTH),
             BMF_DISPLAYABLE | BMF_MINPLANES, scr->RastPort.BitMap);
    if (!bm) { NOTE("AllocBitMap failed"); goto out; }

    /* deterministic "never synced" content */
    {
        struct RastPort prp;
        IGraphics->InitRastPort(&prp);
        prp.BitMap = bm;
        IGraphics->RectFillColor(&prp, 0, 0, VW - 1, VH - 1, PREFILL);
    }

    W3DBase = IExec->OpenLibrary("Warp3D.library", 0);
    IW3D = W3DBase ? (struct Warp3DIFace *)
                     IExec->GetInterface(W3DBase, "main", 1, NULL) : NULL;
    if (!IW3D) { NOTE("no Warp3D main interface"); goto out; }

    CGXBase = IExec->OpenLibrary("cybergraphics.library", 43);
    ICyberGfx = CGXBase ? (struct CyberGfxIFace *)
                          IExec->GetInterface(CGXBase, "main", 1, NULL) : NULL;
    if (!ICyberGfx) { NOTE("no cybergraphics interface"); goto out; }

    {
        struct TagItem cctags[5];
        cctags[0].ti_Tag = W3D_CC_MODEID;     cctags[0].ti_Data =
            IGraphics->GetVPModeID(&scr->ViewPort);
        cctags[1].ti_Tag = W3D_CC_BITMAP;     cctags[1].ti_Data = (uint32)bm;
        cctags[2].ti_Tag = W3D_CC_YOFFSET;    cctags[2].ti_Data = 0;
        cctags[3].ti_Tag = W3D_CC_DRIVERTYPE; cctags[3].ti_Data = W3D_DRIVER_BEST;
        cctags[4].ti_Tag = TAG_DONE;          cctags[4].ti_Data = 0;
        ctx = IW3D->W3D_CreateContext(&err, cctags);
    }
    if (!ctx) { NOTE("CreateContext failed err=%lu", (unsigned long)err); goto out; }
    NOTE("ctx %lux%lu on %dx%d drawregion bitmap, draw=%s, blit=%s, frames=%d",
         (unsigned long)ctx->width, (unsigned long)ctx->height, VW, VH,
         use_elements ? "DrawElements" : "DrawArray(MiniGL-style)",
         do_lock ? "LOCK+blit" : "plain blit", frames);

    {
        W3D_Scissor sc;
        sc.left = 0; sc.top = 0; sc.width = VW; sc.height = VH;
        IW3D->W3D_SetDrawRegion(ctx, bm, 0, &sc);
    }
    IW3D->W3D_SetState(ctx, W3D_GOURAUD,       W3D_ENABLE);
    IW3D->W3D_SetState(ctx, W3D_TEXMAPPING,    W3D_DISABLE);
    IW3D->W3D_SetState(ctx, W3D_ZBUFFER,       W3D_DISABLE);
    IW3D->W3D_SetState(ctx, W3D_ZBUFFERUPDATE, W3D_DISABLE);
    IW3D->W3D_SetState(ctx, W3D_BLENDING,      W3D_DISABLE);

    for (frame = 0; frame < frames; frame++) {
        uint32 drc;
        struct IntuiMessage *msg;
        BOOL closed = FALSE;

        /* rotate */
        {
            float nx = ux * RC_ - uy * RS_;
            float ny = ux * RS_ + uy * RC_;
            ux = nx; uy = ny;
        }

        IW3D->W3D_ClearDrawRegion(ctx, CLEARCOL);
        drc = draw_triangle(VW / 2.0f, VH / 2.0f, ux, uy, 180.0f);
        if (drc != W3D_SUCCESS && frame == 0)
            NOTE("draw rc=%lu", (unsigned long)drc);
        IW3D->W3D_FlushFrame(ctx);
        IW3D->W3D_WaitIdle(ctx);

        if (do_lock)
            sync_bitmap();          /* the one-line difference under test */

        IGraphics->BltBitMapRastPort(bm, 0, 0, win->RPort,
                                     win->BorderLeft, win->BorderTop,
                                     VW, VH, 0xC0);

        if ((frame % 60) == 0) {
            centre = ICyberGfx->ReadRGBPixel(win->RPort,
                                             win->BorderLeft + VW / 2,
                                             win->BorderTop + VH / 2);
            NOTE("frame %3d: window centre pixel = 0x%08lx",
                 frame, (unsigned long)centre);
        }

        while ((msg = (struct IntuiMessage *)IExec->GetMsg(win->UserPort))) {
            if (msg->Class == IDCMP_CLOSEWINDOW) closed = TRUE;
            IExec->ReplyMsg((struct Message *)msg);
        }
        if (closed) { NOTE("close gadget at frame %d", frame); break; }
    }

    /* verdict from the last on-screen centre pixel: prefill grey means the
     * GPU result never reached the window */
    centre = ICyberGfx->ReadRGBPixel(win->RPort,
                                     win->BorderLeft + VW / 2,
                                     win->BorderTop + VH / 2);
    {
        /* the triangle centre is the RGB gouraud centroid ~ (85,85,85);
         * the clear is 0x000060; the prefill sentinel is 0x404040 */
        int r = (centre >> 16) & 0xFF, g = (centre >> 8) & 0xFF, b = centre & 0xFF;
        BOOL prefill  = (r > 0x30 && r < 0x50 && g > 0x30 && g < 0x50 &&
                         b > 0x30 && b < 0x50);
        BOOL clearcol = (r < 0x20 && g < 0x20 && b > 0x40 && b < 0x80);
        BOOL triangle = (r > 0x40 && r < 0x70 && g > 0x40 && g < 0x70 &&
                         b > 0x40 && b < 0x70);
        if (prefill) {
            NOTE("VERDICT: PRESENT BROKEN -- centre 0x%08lx is the prefill "
                 "sentinel; nothing the GPU did reached the window",
                 (unsigned long)centre);
            rc = 5;
        } else if (triangle) {
            NOTE("VERDICT: ALL OK -- centre 0x%08lx is the gouraud centroid; "
                 "draw + present both work", (unsigned long)centre);
            rc = 0;
        } else if (clearcol) {
            NOTE("VERDICT: DRAWS LOST -- clear reached the window but the "
                 "triangle did not (centre 0x%08lx). This is the MiniGL "
                 "symptom: draw path rejects/loses the geometry",
                 (unsigned long)centre);
            rc = 6;
        } else {
            NOTE("VERDICT: unexpected centre 0x%08lx -- inspect manually",
                 (unsigned long)centre);
            rc = 7;
        }
    }

out:
    if (ctx)      IW3D->W3D_DestroyContext(ctx);
    if (ICyberGfx) IExec->DropInterface((struct Interface *)ICyberGfx);
    if (CGXBase)  IExec->CloseLibrary(CGXBase);
    if (IW3D)     IExec->DropInterface((struct Interface *)IW3D);
    if (W3DBase)  IExec->CloseLibrary(W3DBase);
    if (bm)       IGraphics->FreeBitMap(bm);
    if (win)      IIntuition->CloseWindow(win);
    if (scr)      IIntuition->UnlockPubScreen(NULL, scr);
    return rc;
}
