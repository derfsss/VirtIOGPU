/*
 * test_composite.c — Exercise CompositeTags to verify HW compositing hook.
 *
 * Opens a window and composites colored rectangles directly onto it
 * using several Porter-Duff operators.  Waits for close gadget so
 * you can see the results.
 *
 * Run from a Shell: AmigaOS:temp/test_composite
 *
 * Build: ppc-amigaos-gcc -O2 -Wall test_composite.c -o test_composite -lauto
 */

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <graphics/composite.h>
#include <intuition/intuitionbase.h>

#define RECT_W  80
#define RECT_H  80

static void print_result(const char *name, uint32 err)
{
    IDOS->Printf("  %s: %s (%lu)\n", name,
        err == 0 ? "OK" : (err == 3 ? "SW-fallback" : "FAIL"),
        (unsigned long)err);
}

int main(void)
{
    struct Screen *scr = IIntuition->LockPubScreen(NULL);
    if (!scr) {
        IDOS->Printf("Cannot lock Workbench screen\n");
        return 1;
    }

    /* Open a large window */
    struct Window *win = IIntuition->OpenWindowTags(NULL,
        WA_Title,       "CompositeTags HW Test",
        WA_Left,        50,
        WA_Top,         50,
        WA_Width,       500,
        WA_Height,      300,
        WA_DragBar,     TRUE,
        WA_CloseGadget, TRUE,
        WA_DepthGadget, TRUE,
        WA_Activate,    TRUE,
        WA_SmartRefresh, TRUE,
        WA_IDCMP,       IDCMP_CLOSEWINDOW,
        WA_PubScreen,   scr,
        TAG_DONE);

    if (!win) {
        IIntuition->UnlockPubScreen(NULL, scr);
        IDOS->Printf("Cannot open window\n");
        return 1;
    }

    struct BitMap *win_bm = win->RPort->BitMap;
    int32 bx = win->BorderLeft;
    int32 by = win->BorderTop;

    /* Fill window interior with dark grey background */
    IGraphics->SetRPAttrs(win->RPort,
        RPTAG_APenColor, 0xFF404040,
        TAG_DONE);
    IGraphics->RectFill(win->RPort, bx, by,
        win->Width - win->BorderRight - 1,
        win->Height - win->BorderBottom - 1);

    /* Create source bitmap — 32bpp ARGB with semi-transparent red */
    struct BitMap *src_bm = IGraphics->AllocBitMapTags(RECT_W, RECT_H, 32,
        BMATags_Friend,      scr->RastPort.BitMap,
        BMATags_PixelFormat, PIXF_A8R8G8B8,
        BMATags_Displayable, FALSE,
        TAG_DONE);

    if (src_bm) {
        struct RastPort rp;
        IGraphics->InitRastPort(&rp);
        rp.BitMap = src_bm;
        IGraphics->SetRPAttrs(&rp,
            RPTAG_APenColor, 0x80FF0000,  /* semi-transparent red */
            TAG_DONE);
        IGraphics->RectFill(&rp, 0, 0, RECT_W-1, RECT_H-1);
    }

    IDOS->Printf("\nCompositeTags HW Test\n");
    IDOS->Printf("=====================\n");
    IDOS->Printf("Drawing onto window bitmap %p\n\n", win_bm);

    uint32 err;

    /* Column 1: Solid color Src (opaque orange block) */
    err = IGraphics->CompositeTags(
        COMPOSITE_Src,
        COMPSRC_SOLIDCOLOR,
        win_bm,
        COMPTAG_DestX,      bx + 10,
        COMPTAG_DestY,      by + 10,
        COMPTAG_DestWidth,  RECT_W,
        COMPTAG_DestHeight, RECT_H,
        COMPTAG_Color0,     0xFFFF8000,
        TAG_DONE);
    print_result("Solid Src (orange)", err);

    /* Column 2: Solid color SrcOverDest (semi-transparent green over grey) */
    err = IGraphics->CompositeTags(
        COMPOSITE_Src_Over_Dest,
        COMPSRC_SOLIDCOLOR,
        win_bm,
        COMPTAG_DestX,      bx + 100,
        COMPTAG_DestY,      by + 10,
        COMPTAG_DestWidth,  RECT_W,
        COMPTAG_DestHeight, RECT_H,
        COMPTAG_Color0,     0x8000FF00,
        TAG_DONE);
    print_result("Solid SrcOver (green)", err);

    /* Column 3: Solid Plus (additive cyan) */
    err = IGraphics->CompositeTags(
        COMPOSITE_Plus,
        COMPSRC_SOLIDCOLOR,
        win_bm,
        COMPTAG_DestX,      bx + 190,
        COMPTAG_DestY,      by + 10,
        COMPTAG_DestWidth,  RECT_W,
        COMPTAG_DestHeight, RECT_H,
        COMPTAG_Color0,     0x800080FF,
        TAG_DONE);
    print_result("Solid Plus (cyan)", err);

    /* Column 4: Bitmap SrcOverDest (semi-transparent red over grey) */
    if (src_bm) {
        err = IGraphics->CompositeTags(
            COMPOSITE_Src_Over_Dest,
            src_bm,
            win_bm,
            COMPTAG_SrcX,       0,
            COMPTAG_SrcY,       0,
            COMPTAG_SrcWidth,   RECT_W,
            COMPTAG_SrcHeight,  RECT_H,
            COMPTAG_DestX,      bx + 280,
            COMPTAG_DestY,      by + 10,
            COMPTAG_DestWidth,  RECT_W,
            COMPTAG_DestHeight, RECT_H,
            TAG_DONE);
        print_result("Bitmap SrcOver (red)", err);
    }

    /* Column 5: Solid Clear (should make transparent/black) */
    err = IGraphics->CompositeTags(
        COMPOSITE_Clear,
        COMPSRC_SOLIDCOLOR,
        win_bm,
        COMPTAG_DestX,      bx + 370,
        COMPTAG_DestY,      by + 10,
        COMPTAG_DestWidth,  RECT_W,
        COMPTAG_DestHeight, RECT_H,
        COMPTAG_Color0,     0x00000000,
        TAG_DONE);
    print_result("Solid Clear (black)", err);

    /* Row 2: More operators */

    /* Src_In_Dest */
    err = IGraphics->CompositeTags(
        COMPOSITE_Src_In_Dest,
        COMPSRC_SOLIDCOLOR,
        win_bm,
        COMPTAG_DestX,      bx + 10,
        COMPTAG_DestY,      by + 110,
        COMPTAG_DestWidth,  RECT_W,
        COMPTAG_DestHeight, RECT_H,
        COMPTAG_Color0,     0xFFFF00FF,
        TAG_DONE);
    print_result("Solid SrcInDest (magenta)", err);

    /* Xor */
    err = IGraphics->CompositeTags(
        COMPOSITE_Src_Xor_Dest,
        COMPSRC_SOLIDCOLOR,
        win_bm,
        COMPTAG_DestX,      bx + 100,
        COMPTAG_DestY,      by + 110,
        COMPTAG_DestWidth,  RECT_W,
        COMPTAG_DestHeight, RECT_H,
        COMPTAG_Color0,     0x80FFFF00,
        TAG_DONE);
    print_result("Solid Xor (yellow)", err);

    /* Dest_Over_Src */
    err = IGraphics->CompositeTags(
        COMPOSITE_Dest_Over_Src,
        COMPSRC_SOLIDCOLOR,
        win_bm,
        COMPTAG_DestX,      bx + 190,
        COMPTAG_DestY,      by + 110,
        COMPTAG_DestWidth,  RECT_W,
        COMPTAG_DestHeight, RECT_H,
        COMPTAG_Color0,     0x80FF0080,
        TAG_DONE);
    print_result("Solid DestOverSrc (pink)", err);

    IDOS->Printf("\nAll tests done. Results visible in window.\n");
    IDOS->Printf("Check serial log for 'composite:' lines.\n");
    IDOS->Printf("Close window to exit.\n");

    /* Wait for close gadget */
    BOOL running = TRUE;
    while (running) {
        IExec->WaitPort(win->UserPort);
        struct IntuiMessage *msg;
        while ((msg = (struct IntuiMessage *)IExec->GetMsg(win->UserPort))) {
            if (msg->Class == IDCMP_CLOSEWINDOW)
                running = FALSE;
            IExec->ReplyMsg((struct Message *)msg);
        }
    }

    if (src_bm) IGraphics->FreeBitMap(src_bm);
    IIntuition->CloseWindow(win);
    IIntuition->UnlockPubScreen(NULL, scr);
    return 0;
}
