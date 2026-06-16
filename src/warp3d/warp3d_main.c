/*
 * warp3d_main.c -- Warp3D V5 API implementation over the virgl 3D pipeline.
 *
 * Milestone 1: render triangles via the chip's existing virgl pipeline
 * (position+colour VS + per-vertex-colour FS + DRAW_VBO), reached through the
 * chip's "v3d" transport.  W3D_Vertex window coordinates are transformed to
 * NDC on the CPU and packed into the pos[4]+colour[4] stride-32 layout the
 * chip's vertex-elements object expects.
 *
 * Newlib discipline (same as the chip): no %f in debug; no fully-constant
 * non-static float arrays (they synthesise a memcpy -> __NewlibCall ->
 * undefined INewlib at link).  The vertex arrays here are filled at runtime.
 */
#include "warp3d_internal.h"
#include <stdarg.h>

/* Dependency-free tag scan (GetTagData lives in IUtility, which we don't
 * open).  Handles TAG_DONE/TAG_END, TAG_IGNORE, TAG_SKIP and TAG_MORE. */
static uint32 w3d_tagdata(struct TagItem *tags, uint32 tag, uint32 def)
{
    struct TagItem *t = tags;
    while (t) {
        switch (t->ti_Tag) {
            case TAG_DONE:                       return def;
            case TAG_IGNORE:  t++;               continue;
            case TAG_MORE:    t = (struct TagItem *)t->ti_Data; continue;
            case TAG_SKIP:    t += 1 + t->ti_Data; continue;
            default:
                if (t->ti_Tag == tag) return (uint32)t->ti_Data;
                t++;                             continue;
        }
    }
    return def;
}

/* ----------------------------------------------------------------------- */
/* Vertex packing: W3D_Vertex (window coords) -> pos[4]+colour[4] NDC floats */
/* ----------------------------------------------------------------------- */
static void pack_vertex(float *out, const W3D_Vertex *v,
                        float fb_w, float fb_h)
{
    /* window -> NDC.  The render-into-RT + composite-BLIT path is one vertical
     * flip relative to the chip's direct-to-scanout render, so window Y maps
     * straight through (no extra flip here) to land upright after the blit. */
    out[0] = 2.0f * v->x / fb_w - 1.0f;   /* ndc x */
    out[1] = 2.0f * v->y / fb_h - 1.0f;   /* ndc y */
    out[2] = 0.0f;                        /* ndc z (M1: flat) */
    out[3] = 1.0f;                        /* w */
    out[4] = v->color.r;
    out[5] = v->color.g;
    out[6] = v->color.b;
    out[7] = v->color.a;
}

/* Local copy of the chip's INLINE_WRITE-of-floats helper (uses the chip-free
 * inline emitters from virgl_cmd.h).  Each float is GP32-swapped via
 * virgl_emit_float so IEEE-754 bits reach the LE host correctly. */
static void upload_vertex_floats(struct VirglCmdBuf *cbuf, uint32 res_handle,
                                 const float *data, uint32 num_floats)
{
    uint32 data_bytes = num_floats * 4;
    uint32 payload_len = 11 + num_floats;
    uint32 i;

    virgl_emit_dword(cbuf, VIRGL_CMD_HDR(VIRGL_CCMD_RESOURCE_INLINE_WRITE,
                                         0, payload_len));
    virgl_emit_dword(cbuf, res_handle);
    virgl_emit_dword(cbuf, 0);            /* level */
    virgl_emit_dword(cbuf, 0);            /* usage */
    virgl_emit_dword(cbuf, 0);            /* stride */
    virgl_emit_dword(cbuf, 0);            /* layer_stride */
    virgl_emit_dword(cbuf, 0);            /* x = byte offset */
    virgl_emit_dword(cbuf, 0);            /* y */
    virgl_emit_dword(cbuf, 0);            /* z */
    virgl_emit_dword(cbuf, data_bytes);   /* w = size in bytes */
    virgl_emit_dword(cbuf, 1);            /* h */
    virgl_emit_dword(cbuf, 1);            /* d */
    for (i = 0; i < num_floats; i++)
        virgl_emit_float(cbuf, data[i]);
}

/* Bind the context's own render target as the framebuffer.  All warp3d draws
 * target the RT (not the live scanout); the chip composites the RT onto the
 * scanout each frame, so 3D coexists with the desktop without flicker. */
static void bind_rt_framebuffer(struct VirglCmdBuf *cbuf, struct W3DVirgl *wv)
{
    uint32 surf = wv->rt_surface[wv->draw_idx];
    /* colour surface + depth (zsurf) so the depth test has a buffer */
    virgl_cmd_set_framebuffer_state(cbuf, 1, wv->zsurf, &surf);
    /* bind our depth-test DSA (the chip's default DSA has depth off) */
    if (wv->dsa_handle)
        virgl_cmd_bind_object(cbuf, VIRGL_OBJECT_DSA, wv->dsa_handle);
}

/* Frame boundary (called from ClearBuffers/ClearDrawRegion): if geometry was
 * drawn since the last clear, the current back buffer now holds a COMPLETE
 * frame -- register it as the overlay and swap buffers.  Then record the clear
 * colour so the next draw clears the new (now-back) buffer. */
static void frame_clear(struct W3DVirgl *wv, uint32 argb)
{
    if (wv->drawn_since_clear && g_IV3D) {
        g_IV3D->RegisterOverlay(g_IV3D, wv->info.token,
                                wv->rt_res[wv->draw_idx],
                                wv->fb_w, wv->fb_h, 0, 0, wv->fb_w, wv->fb_h, TRUE);
        wv->draw_idx ^= 1;
        wv->drawn_since_clear = FALSE;
    }
    wv->pending_clear = TRUE;
    wv->clear_argb    = argb;
}

/* Encode FB-bind + shaders + upload + set-vbuf + draw into the RT, then submit.
 * verts = nverts * 8 floats (pos[4]+colour[4]).  No scanout flush -- the chip's
 * overlay composite presents the RT.  Returns W3D_SUCCESS/-err. */
static uint32 draw_packed(struct W3DVirgl *wv, const float *verts,
                          uint32 nverts, uint32 pipe_prim)
{
    uint32 cmd_words[256];
    struct VirglCmdBuf cbuf;
    struct VirglVertexBuffer vb;

    virgl_cmd_init(&cbuf, cmd_words, 256);

    bind_rt_framebuffer(&cbuf, wv);

    /* Emit a deferred clear in the SAME command buffer so clear+draw reach the
     * host atomically (the composite never sees a half-drawn RT). */
    if (wv->pending_clear) {
        float cr = (float)((wv->clear_argb >> 16) & 0xFF) / 255.0f;
        float cg = (float)((wv->clear_argb >>  8) & 0xFF) / 255.0f;
        float cb = (float)((wv->clear_argb      ) & 0xFF) / 255.0f;
        float ca = (float)((wv->clear_argb >> 24) & 0xFF) / 255.0f;
        virgl_cmd_clear(&cbuf, 5 /*COLOR0|DEPTH*/, cr, cg, cb, ca, 1.0, 0);
        wv->pending_clear = FALSE;
    }

    /* Ensure the position+colour VS and per-vertex-colour FS are bound. */
    virgl_cmd_bind_shader(&cbuf, PIPE_SHADER_VERTEX,   wv->info.vs_handle);
    virgl_cmd_bind_shader(&cbuf, PIPE_SHADER_FRAGMENT, wv->info.fs_handle);

    upload_vertex_floats(&cbuf, wv->info.vbuf_res, verts, nverts * 8);

    vb.stride        = 32;
    vb.buffer_offset = 0;
    vb.res_handle    = wv->info.vbuf_res;
    virgl_cmd_set_vertex_buffers(&cbuf, 1, &vb);

    virgl_cmd_draw_vbo(&cbuf,
        0, nverts, pipe_prim,   /* start, count, mode */
        0, 1,                   /* indexed, instance_count */
        0, 0,                   /* index_bias, start_instance */
        0, 0,                   /* primitive_restart, restart_index */
        0, nverts - 1,          /* min_index, max_index */
        0);                     /* cso_handle (use bound state) */

    if (!g_IV3D->Submit(g_IV3D, wv->info.token, wv->info.ctx_id,
                        cbuf.buf, cbuf.dwords)) {
        DW3D("draw_packed: Submit FAILED (%lu verts)\n", (unsigned long)nverts);
        return (uint32)-1;
    }
    wv->drawn_since_clear = TRUE;
    return W3D_SUCCESS;
}

static uint32 w3d_pipe_prim(uint32 w3d_prim)
{
    switch (w3d_prim) {
        case W3D_PRIMITIVE_TRIANGLES: return PIPE_PRIM_TRIANGLES;
        case W3D_PRIMITIVE_TRIFAN:    return PIPE_PRIM_TRIANGLE_FAN;
        case W3D_PRIMITIVE_TRISTRIP:  return PIPE_PRIM_TRIANGLE_STRIP;
        case W3D_PRIMITIVE_POINTS:    return PIPE_PRIM_POINTS;
        case W3D_PRIMITIVE_LINES:     return PIPE_PRIM_LINES;
        case W3D_PRIMITIVE_LINESTRIP: return PIPE_PRIM_LINE_STRIP;
        case W3D_PRIMITIVE_LINELOOP:  return PIPE_PRIM_LINE_LOOP;
        default:                      return PIPE_PRIM_TRIANGLES;
    }
}

/* ----------------------------------------------------------------------- */
/* Context lifecycle                                                        */
/* ----------------------------------------------------------------------- */
W3D_Context *w3d_CreateContext(struct Warp3DIFace *Self, uint32 *error,
                               struct TagItem *tags)
{
    W3D_Context     *ctx = NULL;
    struct W3DVirgl *wv  = NULL;
    struct BitMap   *bm;
    (void)Self;

    bm = (struct BitMap *)w3d_tagdata(tags, W3D_CC_BITMAP, 0);
    if (!bm) {
        DW3D("CreateContext: no W3D_CC_BITMAP\n");
        if (error) *error = W3D_ILLEGALINPUT;
        return NULL;
    }

    /* Bring up the chip transport on first use. */
    if (!g_chipBase) {
        g_chipBase = IExec->OpenLibrary("virtiogpu.chip", 0);
        if (g_chipBase)
            g_IV3D = (struct V3DIFace *)IExec->GetInterface(g_chipBase,
                                                            V3D_IFACE_NAME,
                                                            V3D_IFACE_VERSION, NULL);
    }
    if (!g_IV3D) {
        DW3D("CreateContext: chip 'v3d' transport unavailable\n");
        if (error) *error = W3D_NODRIVER;
        return NULL;
    }

    ctx = IExec->AllocVecTags(sizeof(W3D_Context),
                              AVT_Type, MEMF_PRIVATE, AVT_ClearWithValue, 0, TAG_DONE);
    wv  = IExec->AllocVecTags(sizeof(struct W3DVirgl),
                              AVT_Type, MEMF_PRIVATE, AVT_ClearWithValue, 0, TAG_DONE);
    if (!ctx || !wv) {
        if (ctx) IExec->FreeVec(ctx);
        if (wv)  IExec->FreeVec(wv);
        if (error) *error = W3D_NOMEMORY;
        return NULL;
    }

    if (!g_IV3D->ObtainContext(g_IV3D, bm, &wv->info)) {
        DW3D("CreateContext: ObtainContext failed (3D not ready)\n");
        IExec->FreeVec(ctx);
        IExec->FreeVec(wv);
        if (error) *error = W3D_NODRIVER;
        return NULL;
    }

    wv->fb_w = wv->info.fb_width;
    wv->fb_h = wv->info.fb_height;

    /* Heap command buffer for large indexed draws. */
    wv->cmdbuf = IExec->AllocVecTags(W3D_CMDBUF_DWORDS * 4,
                                     AVT_Type, MEMF_PRIVATE, TAG_DONE);
    if (!wv->cmdbuf) {
        IExec->FreeVec(ctx);
        IExec->FreeVec(wv);
        if (error) *error = W3D_NOMEMORY;
        return NULL;
    }

    /* M2/M3: allocate TWO render targets (double-buffer) and register one as
     * the scanout overlay.  warp3d draws into the back buffer; on each frame's
     * clear the completed buffer becomes the overlay and the buffers swap, so
     * the chip only ever composites a complete frame (flicker-free). */
    if (!g_IV3D->AllocRenderTarget(g_IV3D, wv->info.token, wv->fb_w, wv->fb_h,
                                   &wv->rt_res[0], &wv->rt_surface[0]) ||
        !g_IV3D->AllocRenderTarget(g_IV3D, wv->info.token, wv->fb_w, wv->fb_h,
                                   &wv->rt_res[1], &wv->rt_surface[1])) {
        DW3D("CreateContext: AllocRenderTarget failed\n");
        IExec->FreeVec(wv->cmdbuf);
        IExec->FreeVec(ctx);
        IExec->FreeVec(wv);
        if (error) *error = W3D_NOMEMORY;
        return NULL;
    }
    wv->draw_idx = 0;               /* draw into buffer 0 first */
    wv->drawn_since_clear = FALSE;

    /* Shared depth buffer + a depth-test DSA (LESS, write enabled) so the cow
     * surfaces occlude correctly regardless of triangle draw order. */
    if (g_IV3D->AllocDepthBuffer(g_IV3D, wv->info.token, wv->fb_w, wv->fb_h,
                                 &wv->zres, &wv->zsurf)) {
        uint32 dw[16]; struct VirglCmdBuf dcb;
        wv->dsa_handle = 300;       /* warp3d object handle range (>= chip's) */
        virgl_cmd_init(&dcb, dw, 16);
        virgl_cmd_create_dsa(&dcb, wv->dsa_handle,
            VIRGL_DSA_S0_DEPTH_ENABLE(1) | VIRGL_DSA_S0_DEPTH_WRITEMASK(1) |
            VIRGL_DSA_S0_DEPTH_FUNC(PIPE_FUNC_LESS), 0, 0, 0.0f);
        g_IV3D->Submit(g_IV3D, wv->info.token, wv->info.ctx_id, dcb.buf, dcb.dwords);
    }

    /* Clear both buffers (colour+depth) so neither shows garbage initially. */
    {
        uint32 cw[16]; struct VirglCmdBuf cb; int b;
        for (b = 0; b < 2; b++) {
            uint32 surf = wv->rt_surface[b];
            virgl_cmd_init(&cb, cw, 16);
            virgl_cmd_set_framebuffer_state(&cb, 1, wv->zsurf, &surf);
            virgl_cmd_clear(&cb, 5, 0.0f, 0.0f, 0.0f, 1.0f, 1.0, 0);
            g_IV3D->Submit(g_IV3D, wv->info.token, wv->info.ctx_id, cb.buf, cb.dwords);
        }
    }
    /* Show buffer 1 initially (buffer 0 is the first back buffer). */
    g_IV3D->RegisterOverlay(g_IV3D, wv->info.token, wv->rt_res[1],
                            wv->fb_w, wv->fb_h, 0, 0, wv->fb_w, wv->fb_h, TRUE);

    ctx->driver     = wv;
    ctx->drivertype = W3D_DRIVER_3DHW;
    ctx->drawregion = bm;
    ctx->width      = (int)wv->info.fb_width;
    ctx->height     = (int)wv->info.fb_height;

    DW3D("CreateContext: OK ctx=%p %lux%lu (virgl ctx=%lu)\n", (void *)ctx,
         (unsigned long)wv->fb_w, (unsigned long)wv->fb_h,
         (unsigned long)wv->info.ctx_id);
    if (error) *error = W3D_SUCCESS;
    return ctx;
}

/* Varargs wrapper -- build a TagItem array from the inline tags and forward. */
W3D_Context *w3d_CreateContextTags(struct Warp3DIFace *Self, uint32 *error, ...)
{
    struct TagItem tags[33];
    va_list ap;
    int n = 0;

    va_start(ap, error);
    while (n < 32) {
        Tag t = va_arg(ap, Tag);
        tags[n].ti_Tag = t;
        if (t == TAG_DONE) break;
        tags[n].ti_Data = va_arg(ap, uint32);
        n++;
    }
    va_end(ap);
    tags[n].ti_Tag = TAG_DONE;
    tags[n].ti_Data = 0;
    return w3d_CreateContext(Self, error, tags);
}

/* Advertise a single hardware driver so apps that enumerate W3D_GetDrivers()
 * find and select us (and don't crash walking a NULL list). */
W3D_Driver **w3d_GetDrivers(struct Warp3DIFace *Self)
{
    static char         drv_name[] = "VirtIOGPU";
    static W3D_Driver   drv      = { 0 /*ChipID*/, 0xFFFFFFFF /*formats*/,
                                     drv_name, FALSE /*swdriver=HW*/ };
    static W3D_Driver  *drv_list[2] = { &drv, NULL };
    (void)Self;
    return drv_list;
}

void w3d_DestroyContext(struct Warp3DIFace *Self, W3D_Context *ctx)
{
    (void)Self;
    if (!ctx) return;
    if (ctx->driver) {
        struct W3DVirgl *wv = ctx->driver;
        if (g_IV3D) {
            g_IV3D->RegisterOverlay(g_IV3D, wv->info.token, 0,
                                    0, 0, 0, 0, 0, 0, FALSE);
            g_IV3D->FreeRenderTarget(g_IV3D, wv->info.token,
                                     wv->rt_res[0], wv->rt_surface[0]);
            g_IV3D->FreeRenderTarget(g_IV3D, wv->info.token,
                                     wv->rt_res[1], wv->rt_surface[1]);
            if (wv->zres)
                g_IV3D->FreeRenderTarget(g_IV3D, wv->info.token,
                                         wv->zres, wv->zsurf);
            g_IV3D->ReleaseContext(g_IV3D, wv->info.token);
        }
        if (wv->cmdbuf) IExec->FreeVec(wv->cmdbuf);
        IExec->FreeVec(wv);
    }
    IExec->FreeVec(ctx);
}

/* ----------------------------------------------------------------------- */
/* State (mostly inert in M1 -- recorded for later milestones)             */
/* ----------------------------------------------------------------------- */
uint32 w3d_GetState(struct Warp3DIFace *Self, W3D_Context *ctx, uint32 state)
{
    struct W3DVirgl *wv;
    (void)Self;
    if (!ctx || !ctx->driver) return 0;
    wv = ctx->driver;
    return (wv->state & state) ? 1u : 0u;
}

uint32 w3d_SetState(struct Warp3DIFace *Self, W3D_Context *ctx, uint32 state, uint32 action)
{
    struct W3DVirgl *wv;
    (void)Self;
    if (!ctx || !ctx->driver) return W3D_ILLEGALINPUT;
    wv = ctx->driver;
    if (action) wv->state |= state; else wv->state &= ~state;
    ctx->state = wv->state;
    return W3D_SUCCESS;
}

uint32 w3d_CheckDriver(struct Warp3DIFace *Self)
{
    (void)Self;
    /* Advertise a hardware 3D driver so apps/MiniGL pick us. */
    return W3D_DRIVER_3DHW | W3D_DRIVER_BEST;
}

uint32 w3d_LockHardware(struct Warp3DIFace *Self, W3D_Context *ctx)
{
    (void)Self; (void)ctx;
    /* Apps test `if (W3D_SUCCESS != W3D_LockHardware(ctx)) bail;` -- must
     * return W3D_SUCCESS (0) on success, NOT TRUE.  (Serialisation is handled
     * chip-side via io_lock, so this is just a success ack.) */
    return W3D_SUCCESS;
}
void w3d_UnLockHardware(struct Warp3DIFace *Self, W3D_Context *ctx) { (void)Self; (void)ctx; }
void w3d_WaitIdle(struct Warp3DIFace *Self, W3D_Context *ctx)       { (void)Self; (void)ctx; }
uint32 w3d_CheckIdle(struct Warp3DIFace *Self, W3D_Context *ctx)    { (void)Self; (void)ctx; return TRUE; }

uint32 w3d_SetBlendMode(struct Warp3DIFace *Self, W3D_Context *ctx, uint32 s, uint32 d)
{
    struct W3DVirgl *wv;
    (void)Self;
    if (!ctx || !ctx->driver) return W3D_ILLEGALINPUT;
    wv = ctx->driver;
    wv->src_blend = s; wv->dst_blend = d;
    return W3D_SUCCESS;
}

uint32 w3d_SetDrawRegion(struct Warp3DIFace *Self, W3D_Context *ctx,
                         struct BitMap *bm, int yoff, W3D_Scissor *sc)
{
    (void)Self; (void)yoff; (void)sc;
    if (!ctx || !ctx->driver) return W3D_ILLEGALINPUT;
    if (bm) ctx->drawregion = bm;
    return W3D_SUCCESS;
}

uint32 w3d_AllocZBuffer(struct Warp3DIFace *Self, W3D_Context *ctx)
{
    (void)Self;
    if (!ctx) return W3D_ILLEGALINPUT;
    ctx->zbufferalloc = TRUE;   /* Z handled host-side by virgl (M1: noop) */
    return W3D_SUCCESS;
}

/* ----------------------------------------------------------------------- */
/* Drawing                                                                  */
/* ----------------------------------------------------------------------- */
uint32 w3d_DrawTriangle(struct Warp3DIFace *Self, W3D_Context *ctx, W3D_Triangle *tri)
{
    struct W3DVirgl *wv;
    float verts[24];
    (void)Self;

    if (!ctx || !ctx->driver || !tri) return W3D_ILLEGALINPUT;
    wv = ctx->driver;

    pack_vertex(&verts[0],  &tri->v1, (float)wv->fb_w, (float)wv->fb_h);
    pack_vertex(&verts[8],  &tri->v2, (float)wv->fb_w, (float)wv->fb_h);
    pack_vertex(&verts[16], &tri->v3, (float)wv->fb_w, (float)wv->fb_h);

    return draw_packed(wv, verts, 3, PIPE_PRIM_TRIANGLES);
}

uint32 w3d_VertexPointer(struct Warp3DIFace *Self, W3D_Context *ctx,
                         void *p, int stride, uint32 mode, uint32 flags)
{
    struct W3DVirgl *wv;
    (void)Self; (void)flags;
    if (!ctx || !ctx->driver) return W3D_ILLEGALINPUT;
    wv = ctx->driver;
    wv->vtx_ptr    = (const UBYTE *)p;
    wv->vtx_stride = stride ? stride : (int)sizeof(W3D_Vertex);
    wv->vtx_mode   = mode;
    return W3D_SUCCESS;
}

uint32 w3d_ColorPointer(struct Warp3DIFace *Self, W3D_Context *ctx,
                        void *p, int stride, uint32 fmt, uint32 mode, uint32 flags)
{
    struct W3DVirgl *wv;
    (void)Self; (void)mode; (void)flags;
    if (!ctx || !ctx->driver) return W3D_ILLEGALINPUT;
    wv = ctx->driver;
    wv->col_ptr    = (const UBYTE *)p;
    wv->col_stride = stride;
    wv->col_fmt    = fmt;
    return W3D_SUCCESS;
}

/* M1 DrawArray: treats the bound vertex pointer as an array of W3D_Vertex
 * (the common case; full vertex-format decode is a later milestone).  Caps at
 * what the shared 256-byte vbuf holds (8 verts of stride 32). */
uint32 w3d_DrawArray(struct Warp3DIFace *Self, W3D_Context *ctx,
                     uint32 prim, uint32 base, uint32 count)
{
    struct W3DVirgl *wv;
    float verts[64];          /* 8 verts * 8 floats */
    uint32 i;
    (void)Self;

    if (!ctx || !ctx->driver) return W3D_ILLEGALINPUT;
    wv = ctx->driver;
    if (!wv->vtx_ptr || count == 0) return W3D_ILLEGALINPUT;
    if (count > 8) {
        DW3D("DrawArray: count %lu > 8 (M1 vbuf cap) -- clamped\n",
             (unsigned long)count);
        count = 8;
    }

    for (i = 0; i < count; i++) {
        const W3D_Vertex *v = (const W3D_Vertex *)
            (wv->vtx_ptr + (base + i) * (uint32)wv->vtx_stride);
        pack_vertex(&verts[i * 8], v, (float)wv->fb_w, (float)wv->fb_h);
    }
    return draw_packed(wv, verts, count, w3d_pipe_prim(prim));
}

/* ----------------------------------------------------------------------- */
/* Textures -- minimal placeholder (real upload/sampling is task #31).        */
/* Return a non-NULL W3D_Texture so apps that allocate+bind textures proceed   */
/* to drawing geometry; actual sampling isn't wired yet (colour FS is used).   */
/* ----------------------------------------------------------------------- */
W3D_Texture *w3d_AllocTexObj(struct Warp3DIFace *Self, W3D_Context *ctx,
                             uint32 *error, struct TagItem *tags)
{
    W3D_Texture *tex;
    (void)Self; (void)ctx; (void)tags;
    tex = IExec->AllocVecTags(sizeof(W3D_Texture),
                              AVT_Type, MEMF_PRIVATE, AVT_ClearWithValue, 0, TAG_DONE);
    if (!tex) { if (error) *error = W3D_NOMEMORY; return NULL; }
    if (error) *error = W3D_SUCCESS;
    return tex;
}

W3D_Texture *w3d_AllocTexObjTags(struct Warp3DIFace *Self, W3D_Context *ctx,
                                 uint32 *error, ...)
{
    return w3d_AllocTexObj(Self, ctx, error, NULL);
}

void w3d_FreeTexObj(struct Warp3DIFace *Self, W3D_Context *ctx, W3D_Texture *tex)
{
    (void)Self; (void)ctx;
    if (tex) IExec->FreeVec(tex);
}

/* ----------------------------------------------------------------------- */
/* W3D_InterleavedArray + W3D_DrawElements -- the cow demo's OS4 draw path.   */
/* Position is always 3 floats at offset 0; remaining attributes follow in    */
/* VFORMAT bit order.  Positions are screen-space (the app projects them), so  */
/* we map x,y -> NDC and reuse the existing pos[4]+colour[4] VS/FS via a CPU   */
/* gather of the indexed vertices.                                            */
/* ----------------------------------------------------------------------- */
uint32 w3d_InterleavedArray(struct Warp3DIFace *Self, W3D_Context *ctx,
                            void *p, int stride, uint32 format, uint32 flags)
{
    struct W3DVirgl *wv;
    uint32 off;
    (void)Self; (void)flags;

    if (!ctx || !ctx->driver) return W3D_ILLEGALINPUT;
    wv = ctx->driver;

    wv->ia_ptr    = (const UBYTE *)p;
    wv->ia_stride = stride;
    wv->ia_format = format;
    wv->ia_has_color = FALSE;  wv->ia_color_off  = 0;
    wv->ia_has_tcoord = FALSE; wv->ia_tcoord_off = 0;

    /* position = 3 floats @ 0; then attributes in ascending VFORMAT bit order */
    off = 3 * 4;
    if (format & W3D_VFORMAT_FOG)        off += 4;
    if (format & W3D_VFORMAT_COLOR)      { wv->ia_has_color = TRUE; wv->ia_color_off = off; off += 16; }
    else if (format & W3D_VFORMAT_PACK_COLOR) { off += 4; }  /* packed RGBA -- unsupported colour for now */
    if (format & W3D_VFORMAT_SCOLOR)     off += 16;
    else if (format & W3D_VFORMAT_PACK_SCOLOR) off += 4;
    if (format & W3D_VFORMAT_TCOORD_0)   { wv->ia_has_tcoord = TRUE; wv->ia_tcoord_off = off; off += 8; }

    return W3D_SUCCESS;
}

/* Emit a chunk of gathered, screen->NDC-converted vertices into cbuf as an
 * INLINE_WRITE, then bind vbuf + draw.  Returns FALSE on submit failure. */
static BOOL draw_elements_chunk(struct W3DVirgl *wv, const UBYTE *idx_base,
                                uint32 idx_size, uint32 first, uint32 nverts,
                                uint32 pipe_prim)
{
    struct VirglCmdBuf cbuf;
    struct VirglVertexBuffer vb;
    float fbw = (float)wv->fb_w, fbh = (float)wv->fb_h;
    uint32 num_floats = nverts * 8;
    uint32 i;

    virgl_cmd_init(&cbuf, wv->cmdbuf, W3D_CMDBUF_DWORDS);
    bind_rt_framebuffer(&cbuf, wv);

    if (wv->pending_clear) {
        float cr = (float)((wv->clear_argb >> 16) & 0xFF) / 255.0f;
        float cg = (float)((wv->clear_argb >>  8) & 0xFF) / 255.0f;
        float cb = (float)((wv->clear_argb      ) & 0xFF) / 255.0f;
        float ca = (float)((wv->clear_argb >> 24) & 0xFF) / 255.0f;
        virgl_cmd_clear(&cbuf, 5, cr, cg, cb, ca, 1.0, 0);
        wv->pending_clear = FALSE;
    }

    virgl_cmd_bind_shader(&cbuf, PIPE_SHADER_VERTEX,   wv->info.vs_handle);
    virgl_cmd_bind_shader(&cbuf, PIPE_SHADER_FRAGMENT, wv->info.fs_handle);

    /* INLINE_WRITE header (11 words) for the vbuf, then the gathered floats. */
    virgl_emit_dword(&cbuf, VIRGL_CMD_HDR(VIRGL_CCMD_RESOURCE_INLINE_WRITE, 0,
                                          11 + num_floats));
    virgl_emit_dword(&cbuf, wv->info.vbuf_res);
    virgl_emit_dword(&cbuf, 0); virgl_emit_dword(&cbuf, 0);
    virgl_emit_dword(&cbuf, 0); virgl_emit_dword(&cbuf, 0);
    virgl_emit_dword(&cbuf, 0); virgl_emit_dword(&cbuf, 0); virgl_emit_dword(&cbuf, 0);
    virgl_emit_dword(&cbuf, num_floats * 4); /* w = byte size */
    virgl_emit_dword(&cbuf, 1); virgl_emit_dword(&cbuf, 1);

    for (i = 0; i < nverts; i++) {
        uint32 idx;
        const UBYTE *v;
        if      (idx_size == 4) idx = ((const uint32 *)idx_base)[first + i];
        else if (idx_size == 2) idx = ((const UWORD  *)idx_base)[first + i];
        else                    idx = ((const UBYTE  *)idx_base)[first + i];
        v = wv->ia_ptr + idx * (uint32)wv->ia_stride;

        {
            float x = *(const float *)(v + 0);
            float y = *(const float *)(v + 4);
            float z = *(const float *)(v + 8);   /* screen z (~[0,0.8]) */
            virgl_emit_float(&cbuf, 2.0f * x / fbw - 1.0f);   /* ndc x */
            virgl_emit_float(&cbuf, 2.0f * y / fbh - 1.0f);   /* ndc y */
            virgl_emit_float(&cbuf, z);                       /* ndc z -> depth */
            virgl_emit_float(&cbuf, 1.0f);                    /* w */
        }
        if (wv->ia_has_color) {
            const float *c = (const float *)(v + wv->ia_color_off);
            virgl_emit_float(&cbuf, c[0]);
            virgl_emit_float(&cbuf, c[1]);
            virgl_emit_float(&cbuf, c[2]);
            virgl_emit_float(&cbuf, c[3]);
        } else {
            virgl_emit_float(&cbuf, 1.0f); virgl_emit_float(&cbuf, 1.0f);
            virgl_emit_float(&cbuf, 1.0f); virgl_emit_float(&cbuf, 1.0f);
        }
    }

    vb.stride = 32; vb.buffer_offset = 0; vb.res_handle = wv->info.vbuf_res;
    virgl_cmd_set_vertex_buffers(&cbuf, 1, &vb);
    virgl_cmd_draw_vbo(&cbuf, 0, nverts, pipe_prim, 0, 1, 0, 0, 0, 0,
                       0, nverts - 1, 0);

    return g_IV3D->Submit(g_IV3D, wv->info.token, wv->info.ctx_id,
                          cbuf.buf, cbuf.dwords);
}

uint32 w3d_DrawElements(struct Warp3DIFace *Self, W3D_Context *ctx,
                        uint32 prim, uint32 type, uint32 count, void *indices)
{
    struct W3DVirgl *wv;
    uint32 idx_size, pipe_prim, done;
    /* Cap each submit so INLINE_WRITE + draw fit the 64 KiB SUBMIT_3D buffer
     * (11 + nverts*8 + overhead <= 16384 dwords).  MUST be a multiple of 3 for
     * TRIANGLES so we never split a triangle across submit chunks (doing so
     * produces garbage triangles spanning the mesh). */
    const uint32 CHUNK = 1998;
    (void)Self;

    if (!ctx || !ctx->driver) return W3D_ILLEGALINPUT;
    wv = ctx->driver;
    if (!wv->ia_ptr || !indices || count == 0) return W3D_ILLEGALINPUT;
    if (!wv->cmdbuf) return W3D_NOMEMORY;

    idx_size  = (type == W3D_INDEX_ULONG) ? 4 : (type == W3D_INDEX_UWORD) ? 2 : 1;
    pipe_prim = w3d_pipe_prim(prim);

    {
        static uint32 once = 0;
        if (!once) {
            once = 1;
            /* log the first vertex's screen coords -> ndc (milli-units, no %f) */
            const W3D_Vertex *dummy = NULL; (void)dummy;
            if (wv->ia_ptr && wv->ia_stride) {
                const UBYTE *v0 = wv->ia_ptr +
                    (idx_size == 4 ? ((const uint32 *)indices)[0] :
                     idx_size == 2 ? ((const UWORD *)indices)[0] :
                                     ((const UBYTE *)indices)[0]) * (uint32)wv->ia_stride;
                float x0 = *(const float *)(v0 + 0);
                float y0 = *(const float *)(v0 + 4);
                DW3D("DrawElements: FIRST prim=%lu count=%lu stride=%ld colorOff=%ld "
                     "fb=%lux%lu v0=(%ld,%ld) ndc*1000=(%ld,%ld)\n",
                     (unsigned long)prim, (unsigned long)count,
                     (long)wv->ia_stride, (long)wv->ia_color_off,
                     (unsigned long)wv->fb_w, (unsigned long)wv->fb_h,
                     (long)x0, (long)y0,
                     (long)((2.0f * x0 / (float)wv->fb_w - 1.0f) * 1000.0f),
                     (long)((2.0f * y0 / (float)wv->fb_h - 1.0f) * 1000.0f));
            }
        }
    }

    for (done = 0; done < count; done += CHUNK) {
        uint32 n = count - done;
        if (n > CHUNK) n = CHUNK;
        if (!draw_elements_chunk(wv, (const UBYTE *)indices, idx_size,
                                 done, n, pipe_prim)) {
            DW3D("DrawElements: chunk submit failed at %lu/%lu\n",
                 (unsigned long)done, (unsigned long)count);
            return (uint32)-1;
        }
        wv->drawn_since_clear = TRUE;
    }
    return W3D_SUCCESS;
}

uint32 w3d_ClearDrawRegion(struct Warp3DIFace *Self, W3D_Context *ctx, uint32 color)
{
    struct W3DVirgl *wv;
    (void)Self;

    if (!ctx || !ctx->driver) return W3D_ILLEGALINPUT;
    wv = ctx->driver;

    /* Frame boundary: swap completed buffer to the overlay, clear the new back. */
    frame_clear(wv, color);
    return W3D_SUCCESS;
}

/* W3D_ClearBuffers: defer a colour clear (depth/stencil clear is task #30). */
uint32 w3d_ClearBuffers(struct Warp3DIFace *Self, W3D_Context *ctx,
                        W3D_Color *color, W3D_Double *depth, uint32 *stencil)
{
    struct W3DVirgl *wv;
    uint32 a, r, g, b;
    (void)Self; (void)depth; (void)stencil;

    if (!ctx || !ctx->driver) return W3D_ILLEGALINPUT;
    wv = ctx->driver;
    if (color) {
        a = (uint32)(color->a * 255.0f) & 0xFF;
        r = (uint32)(color->r * 255.0f) & 0xFF;
        g = (uint32)(color->g * 255.0f) & 0xFF;
        b = (uint32)(color->b * 255.0f) & 0xFF;
        frame_clear(wv, (a << 24) | (r << 16) | (g << 8) | b);
    } else {
        frame_clear(wv, 0xFF000000);
    }
    return W3D_SUCCESS;
}

uint32 w3d_Flush(struct Warp3DIFace *Self, W3D_Context *ctx)
{
    struct W3DVirgl *wv;
    uint32 cmd_words[16];
    struct VirglCmdBuf cbuf;
    (void)Self;

    /* The chip composites our RT onto the scanout every frame, so no explicit
     * present is needed.  Only flush a clear that had no following draw (e.g.
     * a clear-the-screen with no geometry) so it still takes effect. */
    if (!ctx || !ctx->driver) return W3D_ILLEGALINPUT;
    wv = ctx->driver;
    if (wv->pending_clear) {
        float cr = (float)((wv->clear_argb >> 16) & 0xFF) / 255.0f;
        float cg = (float)((wv->clear_argb >>  8) & 0xFF) / 255.0f;
        float cb = (float)((wv->clear_argb      ) & 0xFF) / 255.0f;
        float ca = (float)((wv->clear_argb >> 24) & 0xFF) / 255.0f;
        virgl_cmd_init(&cbuf, cmd_words, 16);
        bind_rt_framebuffer(&cbuf, wv);
        virgl_cmd_clear(&cbuf, 5 /*COLOR0|DEPTH*/, cr, cg, cb, ca, 1.0, 0);
        g_IV3D->Submit(g_IV3D, wv->info.token, wv->info.ctx_id,
                       cbuf.buf, cbuf.dwords);
        wv->pending_clear = FALSE;
    }
    return W3D_SUCCESS;
}

void w3d_FlushFrame(struct Warp3DIFace *Self, W3D_Context *ctx)
{
    (void)Self;
    w3d_Flush(Self, ctx);
}
