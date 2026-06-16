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
    /* window -> NDC.  Window Y grows downward; NDC Y grows up, so flip Y. */
    out[0] = 2.0f * v->x / fb_w - 1.0f;   /* ndc x */
    out[1] = 1.0f - 2.0f * v->y / fb_h;   /* ndc y */
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

/* Encode bind-shaders + upload + set-vbuf + draw, then submit + present.
 * verts = nverts * 8 floats (pos[4]+colour[4]).  Returns W3D_SUCCESS/-err. */
static uint32 draw_packed(struct W3DVirgl *wv, const float *verts,
                          uint32 nverts, uint32 pipe_prim)
{
    uint32 cmd_words[256];
    struct VirglCmdBuf cbuf;
    struct VirglVertexBuffer vb;

    virgl_cmd_init(&cbuf, cmd_words, 256);

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
    g_IV3D->Flush(g_IV3D, wv->info.token, wv->info.scanout_res,
                  0, 0, wv->fb_w, wv->fb_h);
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

void w3d_DestroyContext(struct Warp3DIFace *Self, W3D_Context *ctx)
{
    (void)Self;
    if (!ctx) return;
    if (ctx->driver) {
        if (g_IV3D)
            g_IV3D->ReleaseContext(g_IV3D, ((struct W3DVirgl *)ctx->driver)->info.token);
        IExec->FreeVec(ctx->driver);
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
    return TRUE;   /* serialisation is handled chip-side via io_lock */
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

uint32 w3d_ClearDrawRegion(struct Warp3DIFace *Self, W3D_Context *ctx, uint32 color)
{
    struct W3DVirgl *wv;
    uint32 cmd_words[16];
    struct VirglCmdBuf cbuf;
    float r, g, b, a;
    (void)Self;

    if (!ctx || !ctx->driver) return W3D_ILLEGALINPUT;
    wv = ctx->driver;

    /* color = 0xAARRGGBB */
    a = (float)((color >> 24) & 0xFF) / 255.0f;
    r = (float)((color >> 16) & 0xFF) / 255.0f;
    g = (float)((color >>  8) & 0xFF) / 255.0f;
    b = (float)((color      ) & 0xFF) / 255.0f;

    virgl_cmd_init(&cbuf, cmd_words, 16);
    virgl_cmd_clear(&cbuf, 4 /*PIPE_CLEAR_COLOR0*/, r, g, b, a, 1.0, 0);
    if (!g_IV3D->Submit(g_IV3D, wv->info.token, wv->info.ctx_id,
                        cbuf.buf, cbuf.dwords))
        return (uint32)-1;
    g_IV3D->Flush(g_IV3D, wv->info.token, wv->info.scanout_res,
                  0, 0, wv->fb_w, wv->fb_h);
    return W3D_SUCCESS;
}

uint32 w3d_Flush(struct Warp3DIFace *Self, W3D_Context *ctx)
{
    struct W3DVirgl *wv;
    (void)Self;
    if (!ctx || !ctx->driver) return W3D_ILLEGALINPUT;
    wv = ctx->driver;
    g_IV3D->Flush(g_IV3D, wv->info.token, wv->info.scanout_res,
                  0, 0, wv->fb_w, wv->fb_h);
    return W3D_SUCCESS;
}

void w3d_FlushFrame(struct Warp3DIFace *Self, W3D_Context *ctx)
{
    (void)Self;
    w3d_Flush(Self, ctx);
}
