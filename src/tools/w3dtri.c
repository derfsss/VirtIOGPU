/*
 * w3dtri.c -- minimal Warp3D triangle test client.
 *
 * Opens warp3d.library, creates a context bound to the Workbench screen
 * bitmap, and draws an RGB-corner triangle in a short loop (redrawn each
 * iteration so it stays visible against the chip's periodic full-frame
 * present).  Proves the warp3d.library -> "v3d" -> virgl -> host-GL path.
 *
 * Build: normal -lauto executable (uses standard clib).  Run from a shell:
 *   w3dtri
 */
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/dos.h>
#include <intuition/screens.h>
#include <warp3d/warp3d.h>
#include <interfaces/warp3d.h>
#include <string.h>
#include <stdio.h>

static void set_vertex(W3D_Vertex *v, float x, float y,
                       float r, float g, float b)
{
    memset(v, 0, sizeof(*v));
    v->x = x;  v->y = y;  v->w = 1.0f;
    v->color.r = r;  v->color.g = g;  v->color.b = b;  v->color.a = 1.0f;
}

int main(void)
{
    struct Library      *W3DBase;
    struct Warp3DIFace  *IWarp3D;
    struct Screen       *scr;
    struct BitMap       *bm;
    W3D_Context         *ctx;
    W3D_Triangle         tri;
    uint32               err = 0;
    int                  frames = 600, i;
    float                w, h;

    scr = IIntuition->LockPubScreen(NULL);
    if (!scr) { printf("w3dtri: no public screen\n"); return 20; }
    bm = scr->RastPort.BitMap;
    w  = (float)scr->Width;
    h  = (float)scr->Height;

    W3DBase = IExec->OpenLibrary("Warp3D.library", 0);
    if (!W3DBase) {
        printf("w3dtri: can't open warp3d.library\n");
        IIntuition->UnlockPubScreen(NULL, scr);
        return 20;
    }
    IWarp3D = (struct Warp3DIFace *)IExec->GetInterface(W3DBase, "main", 1, NULL);
    if (!IWarp3D) {
        printf("w3dtri: no 'main' interface\n");
        IExec->CloseLibrary(W3DBase);
        IIntuition->UnlockPubScreen(NULL, scr);
        return 20;
    }

    printf("w3dtri: CheckDriver=0x%lx screen=%.0fx%.0f\n",
           (unsigned long)IWarp3D->W3D_CheckDriver(), w, h);

    struct TagItem cctags[] = {
        { W3D_CC_BITMAP,     (Tag)bm                 },
        { W3D_CC_DRIVERTYPE, W3D_DRIVER_BEST         },
        { TAG_DONE,          0                       }
    };
    ctx = IWarp3D->W3D_CreateContext(&err, cctags);
    if (!ctx) {
        printf("w3dtri: W3D_CreateContext failed err=%ld\n", (long)err);
        IExec->DropInterface((struct Interface *)IWarp3D);
        IExec->CloseLibrary(W3DBase);
        IIntuition->UnlockPubScreen(NULL, scr);
        return 20;
    }
    printf("w3dtri: context created (%dx%d) -- drawing %d frames\n",
           ctx->width, ctx->height, frames);

    IWarp3D->W3D_SetState(ctx, W3D_GOURAUD, W3D_ENABLE);

    /* RGB-corner triangle in window coordinates. */
    set_vertex(&tri.v1, w * 0.5f, h * 0.15f, 1.0f, 0.0f, 0.0f);  /* top    red   */
    set_vertex(&tri.v2, w * 0.2f, h * 0.80f, 0.0f, 1.0f, 0.0f);  /* left   green */
    set_vertex(&tri.v3, w * 0.8f, h * 0.80f, 0.0f, 0.0f, 1.0f);  /* right  blue  */
    tri.tex = NULL;
    tri.st_pattern = NULL;

    for (i = 0; i < frames; i++) {
        IWarp3D->W3D_ClearDrawRegion(ctx, 0xFF202840);  /* dark blue-grey bg */
        IWarp3D->W3D_DrawTriangle(ctx, &tri);
        IWarp3D->W3D_Flush(ctx);
        IDOS->Delay(2);   /* ~40ms */
    }

    printf("w3dtri: done\n");
    IWarp3D->W3D_DestroyContext(ctx);
    IExec->DropInterface((struct Interface *)IWarp3D);
    IExec->CloseLibrary(W3DBase);
    IIntuition->UnlockPubScreen(NULL, scr);
    return 0;
}
