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
/* Live-texture magic: the stock FE owns the W3D_Texture and dispatches
 * texture ops through backend slots whose exact arg layout is RE-derived --
 * validating this before touching ti makes a mis-decoded slot call a logged
 * no-op instead of a FreeVec-of-garbage DSI. */
#define W3DTEX_MAGIC 0x57335458   /* 'W3TX' */

struct W3DTexInfo {
    uint32 magic;   /* W3DTEX_MAGIC while valid; cleared on free */
    uint32 res;     /* texture resource id */
    uint32 view;    /* sampler view handle */
    uint32 w, h;
    /* Per-texture W3D_SetTexEnv state (classic W3D texenv is PER-TEXTURE --
     * the FE dispatches it to backend slot 35 and stores NOTHING itself;
     * ctx->globaltexenvmode is only the context default).  0 = never set ->
     * draw falls back to the context global. */
    uint32 texenv_mode;      /* W3D_REPLACE/DECAL/MODULATE/BLEND or 0 */
    float  texenv_color[4];  /* env colour (W3D_BLEND) */
    /* Per-texture filter/wrap (W3D_SetFilter slot 30 / W3D_SetWrapMode slot
     * 32).  A private sampler object is (re)built lazily at the next draw;
     * 0 = defaults -> the chip's shared linear sampler is used. */
    uint32 filter_min, filter_mag;  /* W3D_NEAREST/W3D_LINEAR... or 0 */
    uint32 wrap_s, wrap_t;          /* W3D_REPEAT/W3D_CLAMP_LAST... or 0 */
    float  border[4];               /* border colour (wrap mode 3) */
    uint32 sampler;                 /* live private sampler handle, 0 = none */
    uint32 sampler_dirty;           /* filter/wrap changed -> rebuild */
};

struct W3DVirgl {
    struct V3DContextInfo info;     /* handles into the chip's virgl pipeline */
    uint32 fb_w, fb_h;              /* drawregion dims for window->NDC mapping */
    struct BitMap *bm;              /* the W3D_CC_BITMAP -- present target (backend
                                     * model: no scanout overlay, so every draw
                                     * presents the RT into this app bitmap) */

    /* M2/M3: own DOUBLE-BUFFERED render targets -- warp3d draws into the back
     * buffer; on each frame's clear the completed buffer is registered as the
     * overlay and the buffers swap, so the chip only ever composites a complete
     * frame (no mid-frame flicker). */
    uint32 rt_res[2];               /* render-target resources */
    uint32 rt_surface[2];           /* surface handles (framebuffer bind) */
    uint32 draw_idx;                /* which buffer we currently draw into */
    BOOL   drawn_since_clear;       /* geometry submitted since last frame clear */
    BOOL   rt_dirty;                /* RT has content the app bitmap hasn't seen --
                                     * cleared by frame_present.  Drives the
                                     * WaitIdle/CheckIdle/Flush present hooks for
                                     * clients that never call FlushFrame (MiniGL) */
    uint32 hbase;                   /* per-context virgl object-handle block base
                                     * (64K handles).  All W3D contexts share virgl
                                     * ctx 1, so FIXED handle numbers collide across
                                     * contexts: one context's twin-flip DESTROYS a
                                     * handle another live context still binds ->
                                     * Illegal handle + DRAW_VBO 127 and the host
                                     * context is poisoned for good (soak Bug 1,
                                     * 2026-07-09).  Unique blocks kill the class. */

    /* Shared depth buffer + depth-test DSA (for correct occlusion).  Three DSA
     * variants are pre-created so W3D_SetState(ZBUFFER/ZBUFFERUPDATE) can toggle
     * depth test/write per draw (e.g. the cow's 2D blended overlay disables Z so
     * it composites over the scene). */
    uint32 zres, zsurf;             /* depth resource + surface (0 = none) */
    uint32 dsa_handle;              /* base DSA handle (test+write, == DSA_TW) */
    BOOL   depth_test;              /* W3D_ZBUFFER       (default TRUE) */
    BOOL   depth_write;             /* W3D_ZBUFFERUPDATE (default TRUE) */

    /* Currently bound texture (TMU 0), NULL = untextured (colour) draws. */
    W3D_Texture *cur_tex;

    /* W3D_SetTexEnv combine mode + env colour, read per-draw from the public
     * W3D_Context (globaltexenvmode/globaltexenvcolor).  Default W3D_REPLACE keeps
     * the cow/cosmos on the existing 2-attr path (R8). */
    uint32 texenv_mode;          /* W3D_REPLACE/DECAL/MODULATE/BLEND (1..4) */
    uint32 texenv_mode_logged;   /* last mode reported to serial (transition log
                                  * only -- diagnoses which path a workload runs
                                  * without per-draw spam; 0 = nothing logged) */
    float  texenv_color[4];      /* env colour r,g,b,a (W3D_BLEND -> CONST[0]) */
    BOOL   ve3_bound;            /* TRUE if last draw bound the wide VE3 (stride 48);
                                  * REPLACE rebinds VE only to undo it (keeps the
                                  * cow's REPLACE path byte-identical otherwise). */

    /* Deferred clear: ClearDrawRegion records the colour; the next draw emits
     * clear+draw in ONE submit so the chip's composite never observes the RT
     * cleared-but-not-yet-drawn (which makes the geometry flicker). */
    BOOL   pending_clear;
    uint32 clear_argb;

    /* W3D state mirror */
    uint32 state;                   /* W3D_* enable bits */
    uint32 src_blend, dst_blend;    /* W3D_SetBlendMode funcs */
    BOOL   blend_on;                /* W3D_BLENDING enabled */
    BOOL   blend_funcs_dirty;       /* src/dst changed -> recreate blend obj */

    /* ---- 8b batch 1 dynamic pipeline state.  virgl objects are immutable,
     * so every state change FLIP-FLOPS between two handles: create the new
     * one, bind it, destroy the old -- never create over a live handle and
     * never destroy a bound one. ---- */
    /* alpha test (W3D_SetAlphaMode; enabled by the W3D_ALPHATEST mirror bit) */
    uint32 alpha_func;              /* PIPE_FUNC_*, 0 = never set */
    float  alpha_ref;
    BOOL   alpha_on;                /* fe mirror, per draw */
    uint32 dsa_alpha_h;             /* live alpha-DSA handle (315/317), 0=none */
    uint32 dsa_alpha_s0;            /* S0 the live object was built with */
    float  dsa_alpha_ref;           /* ref the live object was built with */
    /* colour mask (W3D_SetColorMask) -- baked into BOTH blend objects */
    uint32 color_mask;              /* pipe RGBA bits, default 0xF */
    BOOL   blend_mask_dirty;        /* mask changed -> recreate blend objects */
    uint32 blend_opaque_h;          /* live no-blend object (310/313) */
    uint32 blend_func_h;            /* live app-factors object (311/312) */
    /* backface cull (W3D_CULLFACE mirror bit + W3D_SetFrontFace) */
    BOOL   cull_on;                 /* fe mirror, per draw */
    BOOL   front_ccw;               /* W3D_SetFrontFace (default CCW) */
    uint32 rast_h;                  /* live rasterizer handle (316/318), 0=none */
    uint32 rast_s0;                 /* S0 the live object was built with */
    /* per-texture sampler handle allocator (filter/wrap; 340+) */
    uint32 sampler_next;
    /* fog (W3D_SetFogParams + the W3D_FOGGING mirror bit).  Params are read
     * from the public ctx->fog per draw; the MODE arrives via the recorder
     * (default LINEAR).  Factors are CPU-computed per vertex and ride the
     * wide path's texcoord .z; untextured draws pre-mix into the colour. */
    BOOL   fog_on;                  /* fe mirror, per draw */
    uint32 fog_mode;                /* W3D_FOG_LINEAR/EXP/EXP_2/INTERPOLATED */
    float  fog_start, fog_end, fog_density;
    float  fog_color[3];
    /* stencil (W3D_SetStencilFunc/Op/WriteMask; enabled by the
     * W3D_STENCILBUFFER mirror bit; buffer = the Z24S8 depth buffer).
     * Baked into the dynamic DSA (single-face: front == back). */
    BOOL   stencil_on;              /* fe mirror, per draw */
    uint32 st_func;                 /* PIPE_FUNC_*, default ALWAYS */
    uint32 st_ref;                  /* reference value (SET_STENCIL_REF) */
    uint32 st_valuemask, st_writemask;
    uint32 st_fail, st_zfail, st_zpass;  /* PIPE_STENCIL_OP_* */
    uint32 dsa_alpha_s1;            /* stencil word the live DSA was built with */
    /* point sizes / line widths (W3D_DrawPoint/W3D_DrawLine immediates) --
     * baked into the W3D rasterizer object */
    float  point_size, line_width;
    float  rast_psize, rast_lwidth; /* values the live rasterizer carries */

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
    BOOL         ia_has_fog;    uint32 ia_fog_off;   /* W3D_FOG_INTERPOLATED */

    /* Per-draw gather view resolved by w3d_DrawElements: either the
     * interleaved array above, or the SEPARATE V4 pointers the FE records in
     * the public context (VertexPointer/ColorPointer/TexCoordPointer[0] --
     * the MiniGL path; TexCoordPointer support = the old R4 note).  NULL
     * base = attribute absent. */
    const UBYTE *ga_pos;  int ga_pos_st;
    const UBYTE *ga_col;  int ga_col_st;  BOOL ga_col_ubyte;
    const UBYTE *ga_tc;   int ga_tc_st;   uint32 ga_tc_voff;
    const UBYTE *ga_fog;  int ga_fog_st;  /* per-vertex fog coord or NULL */

    /* Heap command buffer for large indexed draws (too big for the stack). */
    uint32      *cmdbuf;        /* CMDBUF_DWORDS words (= 64 KiB) */

    /* timer.device for w3d_WaitIdle CPU yield -- the demo runs flat-out
     * (FrameLimit=0) and calls WaitIdle several times/frame; a real W3D yields
     * while the GPU works, so we micro-sleep to let the desktop/input/flush
     * tasks run (otherwise the single TCG core starves -> frozen cursor). */
    APTR         timer_mp;     /* struct MsgPort *  */
    APTR         timer_io;     /* struct TimeRequest * */

    /* Caller task priority lowered while a context is live (renders flat-out)
     * so the desktop/input/flush tasks always preempt it; restored on destroy. */
    LONG         saved_pri;
    BOOL         pri_lowered;
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
uint32       w3d_SetAlphaMode(struct Warp3DIFace *Self, W3D_Context *ctx, uint32 mode, W3D_Float *refval);
uint32       w3d_SetColorMask(struct Warp3DIFace *Self, W3D_Context *ctx, uint32 r, uint32 g, uint32 b, uint32 a);
uint32       w3d_SetFrontFace(struct Warp3DIFace *Self, W3D_Context *ctx, uint32 dir);
uint32       w3d_SetTexFilter(W3D_Context *ctx, W3D_Texture *tex, uint32 fmin, uint32 fmag);
uint32       w3d_SetTexWrap(W3D_Context *ctx, W3D_Texture *tex, uint32 mode_s, uint32 mode_t, W3D_Color *border);
uint32       w3d_SetFogParams(W3D_Context *ctx, W3D_Fog *params, uint32 mode);
uint32       w3d_SetStencilFunc(W3D_Context *ctx, uint32 func, uint32 refvalue, uint32 mask);
uint32       w3d_SetStencilOp(W3D_Context *ctx, uint32 sfail, uint32 dpfail, uint32 dppass);
uint32       w3d_SetStencilWriteMask(W3D_Context *ctx, uint32 mask);
uint32       w3d_ClearStencil(W3D_Context *ctx, uint32 *clearval);
uint32       w3d_DrawPoint(struct Warp3DIFace *Self, W3D_Context *ctx, W3D_Point *point);
uint32       w3d_DrawLine(struct Warp3DIFace *Self, W3D_Context *ctx, W3D_Line *line);
uint32       w3d_SetDrawRegion(struct Warp3DIFace *Self, W3D_Context *ctx, struct BitMap *bm, int yoff, W3D_Scissor *sc);
uint32       w3d_DrawTriangle(struct Warp3DIFace *Self, W3D_Context *ctx, W3D_Triangle *tri);
uint32       w3d_DrawTriangleV(struct Warp3DIFace *Self, W3D_Context *ctx, W3D_TriangleV *t);
uint32       w3d_DrawTriFan(struct Warp3DIFace *Self, W3D_Context *ctx, W3D_Triangles *t);
uint32       w3d_DrawTriStrip(struct Warp3DIFace *Self, W3D_Context *ctx, W3D_Triangles *t);
uint32       w3d_DrawTriFanV(struct Warp3DIFace *Self, W3D_Context *ctx, W3D_TrianglesV *t);
uint32       w3d_DrawTriStripV(struct Warp3DIFace *Self, W3D_Context *ctx, W3D_TrianglesV *t);
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
uint32       w3d_RealizeTexture(struct Warp3DIFace *Self, W3D_Context *ctx, W3D_Texture *tex);
uint32       w3d_BindTexture(struct Warp3DIFace *Self, W3D_Context *ctx, uint32 tmu, W3D_Texture *tex);

#endif /* WARP3D_INTERNAL_H */
