/*
 * warp3d_internal.h -- private definitions for warp3d.library.
 *
 * warp3d.library implements the AmigaOS 4 Warp3D V5 API on top of the
 * VirtIO-GPU virgl 3D pipeline, using the chip's "v3d" transport interface
 * (see include/v3d/v3d_iface.h) to submit virgl command streams.
 *
 * This component is GPL (derived in spirit from Wazp3D's W3D->Gallium port);
 * it is a SEPARATE binary from virtiogpu.chip, which stays non-GPL and only
 * exposes the thin C-ABI "v3d" transport.
 */
#ifndef WARP3D_INTERNAL_H
#define WARP3D_INTERNAL_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/interfaces.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <utility/tagitem.h>
#include <proto/exec.h>

#include <warp3d/warp3d.h>
#include <interfaces/warp3d.h>

#include "v3d/v3d_iface.h"
#include "virgl/virgl_cmd.h"

/* ---- globals (defined in warp3d_lib.c) ---- */
extern struct ExecIFace *IExec;
extern struct Library   *g_chipBase;   /* virtiogpu.chip */
extern struct V3DIFace  *g_IV3D;       /* chip's "v3d" transport */

/* ---- private per-context driver data (hung off W3D_Context.driver) ---- */
/* Per-texture GPU handles, hung off W3D_Texture.driver. */
struct W3DTexInfo {
    uint32 res;     /* texture resource id */
    uint32 view;    /* sampler view handle */
    uint32 w, h;
};

struct W3DVirgl {
    struct V3DContextInfo info;     /* handles into the chip's virgl pipeline */
    uint32 fb_w, fb_h;              /* drawregion dims for window->NDC mapping */

    /* M2/M3: own DOUBLE-BUFFERED render targets -- warp3d draws into the back
     * buffer; on each frame's clear the completed buffer is registered as the
     * overlay and the buffers swap, so the chip only ever composites a complete
     * frame (no mid-frame flicker). */
    uint32 rt_res[2];               /* render-target resources */
    uint32 rt_surface[2];           /* surface handles (framebuffer bind) */
    uint32 draw_idx;                /* which buffer we currently draw into */
    BOOL   drawn_since_clear;       /* geometry submitted since last frame clear */

    /* Shared depth buffer + depth-test DSA (for correct occlusion). */
    uint32 zres, zsurf;             /* depth resource + surface (0 = none) */
    uint32 dsa_handle;              /* depth-test DSA object handle */

    /* Currently bound texture (TMU 0), NULL = untextured (colour) draws. */
    W3D_Texture *cur_tex;

    /* Deferred clear: ClearDrawRegion records the colour; the next draw emits
     * clear+draw in ONE submit so the chip's composite never observes the RT
     * cleared-but-not-yet-drawn (which makes the geometry flicker). */
    BOOL   pending_clear;
    uint32 clear_argb;

    /* W3D state mirror (mostly inert in milestone 1) */
    uint32 state;                   /* W3D_* enable bits */
    uint32 src_blend, dst_blend;    /* W3D_SetBlendMode funcs */

    /* bound vertex arrays (W3D_VertexPointer / W3D_ColorPointer) */
    const UBYTE *vtx_ptr;  int vtx_stride;  uint32 vtx_mode;
    const UBYTE *col_ptr;  int col_stride;  uint32 col_fmt;

    /* W3D_InterleavedArray (the OS4 draw path) -- one packed array; position is
     * always 3 floats at offset 0, remaining attributes follow in VFORMAT bit
     * order.  We compute the colour offset for CPU vertex gather. */
    const UBYTE *ia_ptr;
    int          ia_stride;
    uint32       ia_format;
    BOOL         ia_has_color;  uint32 ia_color_off;
    BOOL         ia_has_tcoord; uint32 ia_tcoord_off;

    /* Heap command buffer for large indexed draws (too big for the stack). */
    uint32      *cmdbuf;        /* CMDBUF_DWORDS words (= 64 KiB) */
};

#define W3D_CMDBUF_DWORDS  16384   /* 64 KiB -- the chip's SUBMIT_3D cap */

/* ---- debug (one switch; never use %f -> pulls newlib float printf) ---- */
#ifndef W3D_DEBUG
#define W3D_DEBUG 1
#endif
#if W3D_DEBUG
#define DW3D(...) do { if (IExec) IExec->DebugPrintF("[warp3d.library] " __VA_ARGS__); } while (0)
#else
#define DW3D(...) do { } while (0)
#endif

/* ---- the W3D_* implementations (warp3d_main.c) ---- */
W3D_Context *w3d_CreateContext(struct Warp3DIFace *Self, uint32 *error, struct TagItem *tags);
W3D_Context *w3d_CreateContextTags(struct Warp3DIFace *Self, uint32 *error, ...);
W3D_Driver **w3d_GetDrivers(struct Warp3DIFace *Self);
uint32       w3d_ClearBuffers(struct Warp3DIFace *Self, W3D_Context *ctx, W3D_Color *color, W3D_Double *depth, uint32 *stencil);
void         w3d_DestroyContext(struct Warp3DIFace *Self, W3D_Context *ctx);
uint32       w3d_GetState(struct Warp3DIFace *Self, W3D_Context *ctx, uint32 state);
uint32       w3d_SetState(struct Warp3DIFace *Self, W3D_Context *ctx, uint32 state, uint32 action);
uint32       w3d_CheckDriver(struct Warp3DIFace *Self);
uint32       w3d_LockHardware(struct Warp3DIFace *Self, W3D_Context *ctx);
void         w3d_UnLockHardware(struct Warp3DIFace *Self, W3D_Context *ctx);
void         w3d_WaitIdle(struct Warp3DIFace *Self, W3D_Context *ctx);
uint32       w3d_CheckIdle(struct Warp3DIFace *Self, W3D_Context *ctx);
uint32       w3d_SetBlendMode(struct Warp3DIFace *Self, W3D_Context *ctx, uint32 s, uint32 d);
uint32       w3d_SetDrawRegion(struct Warp3DIFace *Self, W3D_Context *ctx, struct BitMap *bm, int yoff, W3D_Scissor *sc);
uint32       w3d_DrawTriangle(struct Warp3DIFace *Self, W3D_Context *ctx, W3D_Triangle *tri);
uint32       w3d_Flush(struct Warp3DIFace *Self, W3D_Context *ctx);
void         w3d_FlushFrame(struct Warp3DIFace *Self, W3D_Context *ctx);
uint32       w3d_ClearDrawRegion(struct Warp3DIFace *Self, W3D_Context *ctx, uint32 color);
uint32       w3d_AllocZBuffer(struct Warp3DIFace *Self, W3D_Context *ctx);
uint32       w3d_VertexPointer(struct Warp3DIFace *Self, W3D_Context *ctx, void *p, int stride, uint32 mode, uint32 flags);
uint32       w3d_ColorPointer(struct Warp3DIFace *Self, W3D_Context *ctx, void *p, int stride, uint32 fmt, uint32 mode, uint32 flags);
uint32       w3d_DrawArray(struct Warp3DIFace *Self, W3D_Context *ctx, uint32 prim, uint32 base, uint32 count);
uint32       w3d_InterleavedArray(struct Warp3DIFace *Self, W3D_Context *ctx, void *p, int stride, uint32 format, uint32 flags);
uint32       w3d_DrawElements(struct Warp3DIFace *Self, W3D_Context *ctx, uint32 prim, uint32 type, uint32 count, void *indices);
W3D_Texture *w3d_AllocTexObj(struct Warp3DIFace *Self, W3D_Context *ctx, uint32 *error, struct TagItem *tags);
W3D_Texture *w3d_AllocTexObjTags(struct Warp3DIFace *Self, W3D_Context *ctx, uint32 *error, ...);
void         w3d_FreeTexObj(struct Warp3DIFace *Self, W3D_Context *ctx, W3D_Texture *tex);
uint32       w3d_BindTexture(struct Warp3DIFace *Self, W3D_Context *ctx, uint32 tmu, W3D_Texture *tex);

#endif /* WARP3D_INTERNAL_H */
