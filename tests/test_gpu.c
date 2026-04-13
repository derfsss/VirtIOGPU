/*
 * test_gpu.c — Phase 2 test program for virtiogpu.device
 *
 * Workbench / Shell application.  Uses -lauto so exec/dos library globals
 * (IExec, IDOS) are opened automatically.
 *
 * Tests performed:
 *   1. Open virtiogpu.device unit 0
 *   2. GPU_CMD_QUERY_FB — print framebuffer geometry
 *   3. Draw a colour test pattern directly into the framebuffer
 *   4. GPU_CMD_FLUSH — push whole screen
 *   5. GPU_CMD_FLUSH — push top-left quarter (rect test)
 *   6. Upload a white crosshair cursor image
 *   7. GPU_CMD_SET_CURSOR_VISIBLE (show)
 *   8. GPU_CMD_MOVE_CURSOR — sweep across screen
 *   9. GPU_CMD_SET_CURSOR_VISIBLE (hide)
 *  10. Close device
 *
 * Build (inside the tests/ directory):
 *   ppc-amigaos-gcc -O2 -Wall -I../include -o test_gpu test_gpu.c -lauto
 */

#include <exec/exec.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/exectags.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

#include "gpu/gpu_cmds.h"

/* -----------------------------------------------------------------------
 * Open / close virtiogpu.device
 * ----------------------------------------------------------------------- */
static struct IOStdReq *open_gpu(uint32 unit)
{
    struct MsgPort *port = (struct MsgPort *)
        IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (!port) {
        printf("AllocSysObject(PORT) failed\n");
        return NULL;
    }

    struct IOStdReq *ioreq = (struct IOStdReq *)
        IExec->AllocSysObjectTags(ASOT_IOREQUEST,
            ASOIOR_ReplyPort, port,
            ASOIOR_Size, sizeof(struct IOStdReq),
            TAG_END);
    if (!ioreq) {
        printf("AllocSysObject(IOREQUEST) failed\n");
        IExec->FreeSysObject(ASOT_PORT, port);
        return NULL;
    }

    if (IExec->OpenDevice("virtiogpu.device", unit,
                          (struct IORequest *)ioreq, 0) != 0) {
        printf("OpenDevice virtiogpu.device unit %lu failed: error %d\n",
               (unsigned long)unit, ioreq->io_Error);
        IExec->FreeSysObject(ASOT_IOREQUEST, ioreq);
        IExec->FreeSysObject(ASOT_PORT, port);
        return NULL;
    }

    printf("virtiogpu.device unit %lu opened OK\n", (unsigned long)unit);
    return ioreq;
}

static void close_gpu(struct IOStdReq *ioreq)
{
    if (!ioreq) return;
    struct MsgPort *port = ioreq->io_Message.mn_ReplyPort;
    IExec->CloseDevice((struct IORequest *)ioreq);
    IExec->FreeSysObject(ASOT_IOREQUEST, ioreq);
    IExec->FreeSysObject(ASOT_PORT, port);
}

/* -----------------------------------------------------------------------
 * Issue a synchronous IOStdReq command
 * ----------------------------------------------------------------------- */
static int do_cmd(struct IOStdReq *ioreq, uint16 cmd,
                  void *data, uint32 length, uint32 offset)
{
    ioreq->io_Command = cmd;
    ioreq->io_Data    = data;
    ioreq->io_Length  = length;
    ioreq->io_Offset  = offset;
    ioreq->io_Actual  = 0;
    ioreq->io_Error   = 0;
    IExec->DoIO((struct IORequest *)ioreq);
    return ioreq->io_Error;
}

/* -----------------------------------------------------------------------
 * Test pattern: vertical colour bars
 * ----------------------------------------------------------------------- */
static void draw_bars(const struct GPU_FBInfo *fb)
{
    uint32 bar_w = fb->width / 8;
    static const uint32 colours[8] = {
        0x00FFFFFF,  /* White   */
        0x00FFFF00,  /* Yellow  */
        0x0000FFFF,  /* Cyan    */
        0x0000FF00,  /* Green   */
        0x00FF00FF,  /* Magenta */
        0x00FF0000,  /* Red     */
        0x000000FF,  /* Blue    */
        0x00000000,  /* Black   */
    };

    uint32 *fb32  = (uint32 *)fb->fb_virt;
    uint32  pitch = fb->stride / 4;  /* pixels per row */

    for (uint32 y = 0; y < fb->height; y++) {
        for (uint32 x = 0; x < fb->width; x++) {
            uint32 bar = x / bar_w;
            if (bar >= 8) bar = 7;
            fb32[y * pitch + x] = colours[bar];
        }
    }
    printf("Colour bars drawn (%lu x %lu)\n",
           (unsigned long)fb->width, (unsigned long)fb->height);
}

/* -----------------------------------------------------------------------
 * Cursor: white crosshair in a 64x64 transparent BGRA image
 * ----------------------------------------------------------------------- */
static void make_crosshair(uint8 pixels[GPU_CURSOR_WIDTH * GPU_CURSOR_HEIGHT * 4])
{
    memset(pixels, 0, GPU_CURSOR_WIDTH * GPU_CURSOR_HEIGHT * 4);

    uint32 cx  = GPU_CURSOR_WIDTH  / 2;
    uint32 cy  = GPU_CURSOR_HEIGHT / 2;
    uint32 arm = 16;

    for (uint32 i = cx - arm; i <= cx + arm; i++) {
        /* Horizontal arm — row cy, column i */
        uint8 *ph = pixels + (cy * GPU_CURSOR_WIDTH + i) * 4;
        ph[0] = 0xFF; ph[1] = 0xFF; ph[2] = 0xFF; ph[3] = 0xFF;
        /* Vertical arm — row i, column cx */
        uint8 *pv = pixels + (i  * GPU_CURSOR_WIDTH + cx) * 4;
        pv[0] = 0xFF; pv[1] = 0xFF; pv[2] = 0xFF; pv[3] = 0xFF;
    }
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(void)
{
    printf("virtiogpu Phase 2 test\n");
    printf("======================\n\n");

    /* 1. Open device */
    struct IOStdReq *ioreq = open_gpu(0);
    if (!ioreq) return RETURN_FAIL;

    /* 2. Query framebuffer */
    struct GPU_FBInfo fb;
    memset(&fb, 0, sizeof(fb));

    int err = do_cmd(ioreq, GPU_CMD_QUERY_FB, &fb, sizeof(fb), 0);
    if (err) {
        printf("GPU_CMD_QUERY_FB failed: %d\n", err);
        close_gpu(ioreq);
        return RETURN_FAIL;
    }
    printf("Framebuffer:\n");
    printf("  Virtual : 0x%08lX\n", (unsigned long)(APTR)fb.fb_virt);
    printf("  Physical: 0x%08lX\n", (unsigned long)fb.fb_phys);
    printf("  Size    : %lu x %lu\n",
           (unsigned long)fb.width, (unsigned long)fb.height);
    printf("  Stride  : %lu bytes/row\n\n", (unsigned long)fb.stride);

    if (!fb.fb_virt) {
        printf("No framebuffer mapped -- aborting\n");
        close_gpu(ioreq);
        return RETURN_FAIL;
    }

    /* 3. Draw colour bars into the framebuffer */
    draw_bars(&fb);

    /* 4. Flush whole screen */
    err = do_cmd(ioreq, GPU_CMD_FLUSH, NULL, 0, 0);
    if (err)
        printf("GPU_CMD_FLUSH (whole screen) failed: %d\n", err);
    else
        printf("Whole-screen flush OK\n");

    /* 5. Flush top-left quarter as a rect test */
    struct GPU_Rect rect = { 0, 0, fb.width / 2, fb.height / 2 };
    err = do_cmd(ioreq, GPU_CMD_FLUSH, &rect, sizeof(rect), 0);
    if (err)
        printf("GPU_CMD_FLUSH (rect) failed: %d\n", err);
    else
        printf("Rect flush OK (0,0 %lux%lu)\n\n",
               (unsigned long)rect.w, (unsigned long)rect.h);

    /* 6. Upload cursor image */
    struct GPU_CursorImage *img =
        (struct GPU_CursorImage *)IExec->AllocVecTags(
            sizeof(struct GPU_CursorImage),
            AVT_Type, MEMF_ANY,
            AVT_ClearWithValue, 0,
            TAG_END);
    if (img) {
        make_crosshair(img->pixels);
        img->hot_x = GPU_CURSOR_WIDTH  / 2;
        img->hot_y = GPU_CURSOR_HEIGHT / 2;
        err = do_cmd(ioreq, GPU_CMD_SET_CURSOR_IMAGE, img, sizeof(*img), 0);
        if (err)
            printf("GPU_CMD_SET_CURSOR_IMAGE failed: %d\n", err);
        else
            printf("Cursor image uploaded (crosshair, hot=%lu,%lu)\n",
                   (unsigned long)img->hot_x, (unsigned long)img->hot_y);
        IExec->FreeVec(img);
    } else {
        printf("AllocVecTags for cursor image failed\n");
    }

    /* 7. Show cursor */
    err = do_cmd(ioreq, GPU_CMD_SET_CURSOR_VISIBLE, NULL, 0, 1);
    if (err)
        printf("GPU_CMD_SET_CURSOR_VISIBLE (show) failed: %d\n", err);
    else
        printf("Cursor visible\n");

    /* 8. Sweep cursor left-to-right across 1/4 height */
    printf("Sweeping cursor...\n");
    {
        struct GPU_CursorPos pos;
        pos.y = fb.height / 4;
        for (uint32 x = 0; x < fb.width; x += 4) {
            pos.x = x;
            do_cmd(ioreq, GPU_CMD_MOVE_CURSOR, &pos, sizeof(pos), 0);
            IExec->Forbid();
            IExec->Permit();  /* yield briefly */
        }
    }
    printf("Cursor sweep done\n\n");

    /* 9. Wait for Return */
    printf("Press Return to finish...\n");
    {
        TEXT buf[4];
        IDOS->FGets(IDOS->Input(), buf, sizeof(buf));
    }

    /* 10. Hide cursor and clean up */
    err = do_cmd(ioreq, GPU_CMD_SET_CURSOR_VISIBLE, NULL, 0, 0);
    if (err)
        printf("GPU_CMD_SET_CURSOR_VISIBLE (hide) failed: %d\n", err);
    else
        printf("Cursor hidden\n");

    close_gpu(ioreq);
    printf("Done.\n");
    return RETURN_OK;
}
