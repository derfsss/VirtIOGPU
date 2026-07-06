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
#include <cybergraphx/cybergraphics.h>
#include <interfaces/cybergraphics.h>
#include <devices/timer.h>
#include <exec/io.h>

/* cybergraphics.library -- used to lock the W3D_CC_BITMAP and read its base
 * address / stride / dims, so we render at the app bitmap's size and present
 * the rendered frame straight into it (the app then blits it to its window). */
static struct Library       *g_CyberGfxBase = NULL;
static struct CyberGfxIFace *g_ICyberGfx    = NULL;

/* ---- gpu.library-routed v3d transport (Phase 3.2b-2) --------------------
 * The same V3DIFace surface the render code below already uses, carried
 * over gpu.library VGB_OP_V3DCALL submits to the 'virtio-gpu' backend
 * instead of the chip's private "v3d" interface. The IGpu submit is
 * synchronous (the caller blocks until the backend op completes), so
 * pointer args inside the call block remain valid throughout. */
#include <libraries/gpu.h>           /* vendored: -I./include/gpulib */
#include <interfaces/gpu.h>
#include <gpulib/virtio_gpu_backend.h>

static struct Library  *g_GpuBase = NULL;
static struct GpuIFace *g_IGpu    = NULL;
static int32            g_gpuVid  = -1;

static int32 v3dgpu_do(uint32 method, struct VgbV3DCall *c)
{
    c->hdr.op  = VGB_OP_V3DCALL;
    c->hdr.arg = method;
    return g_IGpu->GPU_SubmitA(GPU_QUEUE_RENDER, c, sizeof(*c),
               GPU_TAGS({ GPUTAG_Backend, (uint32)g_gpuVid }));
}

static uint32 v3dgpu_Obtain(struct V3DIFace *Self)
{ (void)Self; return 1; }
static uint32 v3dgpu_Release(struct V3DIFace *Self)
{ (void)Self; return 1; }

static BOOL v3dgpu_ObtainContext(struct V3DIFace *Self, struct BitMap *dest,
                                 struct V3DContextInfo *info)
{
    struct VgbV3DCall c; (void)Self; (void)dest;
    c.a[0] = (uint32)info;
    return v3dgpu_do(VGB_V3D_OBTAIN, &c) > 0;
}

static BOOL v3dgpu_Submit(struct V3DIFace *Self, APTR token, uint32 ctx_id,
                          const uint32 *words, uint32 nwords)
{
    struct VgbV3DCall c; (void)Self;
    c.a[0] = (uint32)token; c.a[1] = ctx_id;
    c.a[2] = (uint32)words; c.a[3] = nwords;
    return v3dgpu_do(VGB_V3D_SUBMIT, &c) > 0;
}

static BOOL v3dgpu_Flush(struct V3DIFace *Self, APTR token, uint32 res_id,
                         uint32 x, uint32 y, uint32 w, uint32 h)
{
    struct VgbV3DCall c; (void)Self;
    c.a[0] = (uint32)token; c.a[1] = res_id;
    c.a[2] = x; c.a[3] = y; c.a[4] = w; c.a[5] = h;
    return v3dgpu_do(VGB_V3D_FLUSH, &c) > 0;
}

static void v3dgpu_ReleaseContext(struct V3DIFace *Self, APTR token)
{
    struct VgbV3DCall c; (void)Self;
    c.a[0] = (uint32)token;
    v3dgpu_do(VGB_V3D_RELEASE, &c);
}

static BOOL v3dgpu_AllocRenderTarget(struct V3DIFace *Self, APTR token,
                                     uint32 w, uint32 h,
                                     uint32 *res_out, uint32 *surface_out)
{
    struct VgbV3DCall c; (void)Self;
    c.a[0] = (uint32)token; c.a[1] = w; c.a[2] = h;
    c.a[3] = (uint32)res_out; c.a[4] = (uint32)surface_out;
    return v3dgpu_do(VGB_V3D_ALLOC_RT, &c) > 0;
}

static void v3dgpu_RegisterOverlay(struct V3DIFace *Self, APTR token,
                                   uint32 rt_res, uint32 sw, uint32 sh,
                                   uint32 x, uint32 y, uint32 w, uint32 h,
                                   BOOL enable)
{
    struct VgbV3DCall c; (void)Self;
    c.a[0] = (uint32)token; c.a[1] = rt_res; c.a[2] = sw; c.a[3] = sh;
    c.a[4] = x; c.a[5] = y; c.a[6] = w; c.a[7] = h; c.a[8] = (uint32)enable;
    v3dgpu_do(VGB_V3D_OVERLAY, &c);
}

static void v3dgpu_FreeRenderTarget(struct V3DIFace *Self, APTR token,
                                    uint32 res, uint32 surface)
{
    struct VgbV3DCall c; (void)Self;
    c.a[0] = (uint32)token; c.a[1] = res; c.a[2] = surface;
    v3dgpu_do(VGB_V3D_FREE_RT, &c);
}

static BOOL v3dgpu_AllocDepthBuffer(struct V3DIFace *Self, APTR token,
                                    uint32 w, uint32 h,
                                    uint32 *res_out, uint32 *surface_out)
{
    struct VgbV3DCall c; (void)Self;
    c.a[0] = (uint32)token; c.a[1] = w; c.a[2] = h;
    c.a[3] = (uint32)res_out; c.a[4] = (uint32)surface_out;
    return v3dgpu_do(VGB_V3D_ALLOC_Z, &c) > 0;
}

static BOOL v3dgpu_CreateTexture(struct V3DIFace *Self, APTR token,
                                 uint32 w, uint32 h, APTR data,
                                 uint32 src_bpr,
                                 uint32 *view_out, uint32 *res_out)
{
    struct VgbV3DCall c; (void)Self;
    c.a[0] = (uint32)token; c.a[1] = w; c.a[2] = h;
    c.a[3] = (uint32)data; c.a[4] = src_bpr;
    c.a[5] = (uint32)view_out; c.a[6] = (uint32)res_out;
    return v3dgpu_do(VGB_V3D_CREATE_TEX, &c) > 0;
}

static void v3dgpu_FreeTexture(struct V3DIFace *Self, APTR token,
                               uint32 res, uint32 view)
{
    struct VgbV3DCall c; (void)Self;
    c.a[0] = (uint32)token; c.a[1] = res; c.a[2] = view;
    v3dgpu_do(VGB_V3D_FREE_TEX, &c);
}

static BOOL v3dgpu_PresentBitmap(struct V3DIFace *Self, APTR token,
                                 uint32 rt_res, uint32 sw, uint32 sh,
                                 APTR dst_base, uint32 dst_stride)
{
    struct VgbV3DCall c; (void)Self;
    c.a[0] = (uint32)token; c.a[1] = rt_res; c.a[2] = sw; c.a[3] = sh;
    c.a[4] = (uint32)dst_base; c.a[5] = dst_stride;
    return v3dgpu_do(VGB_V3D_PRESENT_BM, &c) > 0;
}

static struct V3DIFace v3dgpu_iface = {
    .Obtain            = v3dgpu_Obtain,
    .Release           = v3dgpu_Release,
    .ObtainContext     = v3dgpu_ObtainContext,
    .Submit            = v3dgpu_Submit,
    .Flush             = v3dgpu_Flush,
    .ReleaseContext    = v3dgpu_ReleaseContext,
    .AllocRenderTarget = v3dgpu_AllocRenderTarget,
    .RegisterOverlay   = v3dgpu_RegisterOverlay,
    .FreeRenderTarget  = v3dgpu_FreeRenderTarget,
    .AllocDepthBuffer  = v3dgpu_AllocDepthBuffer,
    .CreateTexture     = v3dgpu_CreateTexture,
    .FreeTexture       = v3dgpu_FreeTexture,
    .PresentBitmap     = v3dgpu_PresentBitmap,
};

static struct CyberGfxIFace *ensure_cybergfx(void)
{
    if (!g_ICyberGfx) {
        if (!g_CyberGfxBase)
            g_CyberGfxBase = IExec->OpenLibrary("cybergraphics.library", 41);
        if (g_CyberGfxBase)
            g_ICyberGfx = (struct CyberGfxIFace *)
                IExec->GetInterface(g_CyberGfxBase, "main", 1, NULL);
    }
    return g_ICyberGfx;
}

/* Read an RTG bitmap's geometry (no lock needed -- the documented query API,
 * which the CoW3D demo itself uses; LockBitMapTagList's LBMI_WIDTH proved
 * unreliable, returning a padded/wrong width). */
static BOOL bitmap_geometry(struct BitMap *bm, uint32 *w, uint32 *h, uint32 *bpr)
{
    struct CyberGfxIFace *cg = ensure_cybergfx();
    if (!cg || !bm) return FALSE;
    if (w)   *w   = (uint32)cg->GetCyberMapAttr(bm, CYBRMATTR_WIDTH);
    if (h)   *h   = (uint32)cg->GetCyberMapAttr(bm, CYBRMATTR_HEIGHT);
    if (bpr) *bpr = (uint32)cg->GetCyberMapAttr(bm, CYBRMATTR_XMOD);
    return TRUE;
}

/* Lock an RTG bitmap for its base address.  *lock_out must be released with
 * ICyberGfx->UnLockBitMap once the caller is done writing to *base. */
static BOOL bitmap_lock_base(struct BitMap *bm, APTR *lock_out, APTR *base)
{
    struct CyberGfxIFace *cg = ensure_cybergfx();
    ULONG vbase = 0;
    struct TagItem qt[] = {
        { LBMI_BASEADDRESS, (Tag)&vbase },
        { TAG_DONE, 0 }
    };
    APTR lock;
    if (!cg || !bm) return FALSE;
    lock = cg->LockBitMapTagList(bm, qt);
    if (!lock) return FALSE;
    *lock_out = lock;
    if (base) *base = (APTR)vbase;
    return TRUE;
}

/* Present the current render target into the app's W3D_CC_BITMAP.  In the BACKEND
 * model there is NO scanout overlay compositing our RT (unlike the custom-lib
 * scanout path), so the ONLY way the window updates is this readback->bitmap.
 * Called after every draw (draw_packed) and on FlushFrame.  Clamp to the bitmap's
 * real geometry so we never write past it. */
static void frame_present(struct W3DVirgl *wv, struct BitMap *bm)
{
    APTR lock, base;
    uint32 bpr = 0, bw = 0, bh = 0, pw, ph;

    if (!g_IV3D || !g_IV3D->PresentBitmap || !bm) return;
    if (!bitmap_geometry(bm, &bw, &bh, &bpr) || !bpr) return;
    pw = wv->fb_w; if (bw && pw > bw) pw = bw; if (pw > bpr / 4) pw = bpr / 4;
    ph = wv->fb_h; if (bh && ph > bh) ph = bh;
    if (!pw || !ph) return;

    if (bitmap_lock_base(bm, &lock, &base)) {
        if (base)
            g_IV3D->PresentBitmap(g_IV3D, wv->info.token, wv->rt_res[wv->draw_idx],
                                  pw, ph, base, bpr);
        g_ICyberGfx->UnLockBitMap(lock);
    }
}

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
    out[1] = 2.0f * v->y / fb_h - 1.0f;   /* ndc y (backend present blit is NOT Y-flipped) */
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
/* W3D_SetBlendMode factor -> Gallium PIPE_BLENDFACTOR. */
static uint32 w3d_blendfactor(uint32 w)
{
    switch (w) {
    case W3D_ZERO:                return PIPE_BLENDFACTOR_ZERO;
    case W3D_ONE:                 return PIPE_BLENDFACTOR_ONE;
    case W3D_SRC_COLOR:           return PIPE_BLENDFACTOR_SRC_COLOR;
    case W3D_DST_COLOR:           return PIPE_BLENDFACTOR_DST_COLOR;
    case W3D_ONE_MINUS_SRC_COLOR: return PIPE_BLENDFACTOR_INV_SRC_COLOR;
    case W3D_ONE_MINUS_DST_COLOR: return PIPE_BLENDFACTOR_INV_DST_COLOR;
    case W3D_SRC_ALPHA:           return PIPE_BLENDFACTOR_SRC_ALPHA;
    case W3D_ONE_MINUS_SRC_ALPHA: return PIPE_BLENDFACTOR_INV_SRC_ALPHA;
    case W3D_DST_ALPHA:           return PIPE_BLENDFACTOR_DST_ALPHA;
    case W3D_ONE_MINUS_DST_ALPHA: return PIPE_BLENDFACTOR_INV_DST_ALPHA;
    case W3D_SRC_ALPHA_SATURATE:  return PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE;
    default:                      return PIPE_BLENDFACTOR_ONE;
    }
}

#define W3D_HANDLE_BLEND_OPAQUE  310   /* COLORMASK only, no blend (+313 twin) */
#define W3D_HANDLE_BLEND_FUNC    311   /* app's W3D_SetBlendMode factors (+312 twin) */
#define W3D_HANDLE_DSA_ALPHA     315   /* depth+alpha-test DSA (+317 twin) */
#define W3D_HANDLE_RAST          316   /* W3D rasterizer: cull/frontface (+318 twin) */

/* virgl objects are immutable: a state change creates the NEW object on the
 * twin handle, binds it, then destroys the old -- never create over a live
 * handle, never destroy a possibly-bound one. */
static uint32 handle_flip(uint32 live, uint32 a, uint32 b)
{
    return (live == a) ? b : a;
}

/* Refresh the blend objects when factors or the colour mask changed, then
 * bind opaque or blend per the W3D_BLENDING enable.  Appended into the draw's
 * command buffer (so create+bind reach the host before the DRAW_VBO). */
static void bind_blend(struct VirglCmdBuf *cbuf, struct W3DVirgl *wv)
{
    uint32 mask = wv->color_mask & 0xF;
    if (wv->blend_funcs_dirty || wv->blend_mask_dirty) {
        uint32 s = w3d_blendfactor(wv->src_blend);
        uint32 d = w3d_blendfactor(wv->dst_blend);
        uint32 rt0 = VIRGL_BLEND_RT_BLEND_ENABLE(1)
                   | VIRGL_BLEND_RT_RGB_FUNC(PIPE_BLEND_ADD)
                   | VIRGL_BLEND_RT_RGB_SRC_FACTOR(s)
                   | VIRGL_BLEND_RT_RGB_DST_FACTOR(d)
                   | VIRGL_BLEND_RT_ALPHA_FUNC(PIPE_BLEND_ADD)
                   | VIRGL_BLEND_RT_ALPHA_SRC_FACTOR(s)
                   | VIRGL_BLEND_RT_ALPHA_DST_FACTOR(d)
                   | VIRGL_BLEND_RT_COLORMASK(mask);
        uint32 nh = handle_flip(wv->blend_func_h, W3D_HANDLE_BLEND_FUNC,
                                W3D_HANDLE_BLEND_FUNC + 1);
        virgl_cmd_create_blend(cbuf, nh, 0, rt0);
        if (wv->blend_func_h)
            virgl_cmd_destroy_object(cbuf, VIRGL_OBJECT_BLEND, wv->blend_func_h);
        wv->blend_func_h = nh;
        wv->blend_funcs_dirty = FALSE;
    }
    if (wv->blend_mask_dirty) {
        uint32 nh = handle_flip(wv->blend_opaque_h, W3D_HANDLE_BLEND_OPAQUE,
                                W3D_HANDLE_BLEND_OPAQUE + 3);
        virgl_cmd_create_blend(cbuf, nh, 0, VIRGL_BLEND_RT_COLORMASK(mask));
        if (wv->blend_opaque_h)
            virgl_cmd_destroy_object(cbuf, VIRGL_OBJECT_BLEND, wv->blend_opaque_h);
        wv->blend_opaque_h = nh;
        wv->blend_mask_dirty = FALSE;
    }
    virgl_cmd_bind_object(cbuf, VIRGL_OBJECT_BLEND,
        wv->blend_on ? wv->blend_func_h : wv->blend_opaque_h);
}

/* Bind the DSA for the current depth + alpha-test state.  Without alpha test
 * the three static objects (300/301/302) cover every depth combination; with
 * it a dynamic object carries depth bits + alpha func + ref (twin-handle
 * recreate only when the effective state actually changed). */
static void bind_dsa(struct VirglCmdBuf *cbuf, struct W3DVirgl *wv)
{
    if (!wv->dsa_handle) return;
    if (wv->alpha_on && wv->alpha_func) {
        uint32 s0 = (wv->depth_test
                       ? (VIRGL_DSA_S0_DEPTH_ENABLE(1) |
                          VIRGL_DSA_S0_DEPTH_WRITEMASK(wv->depth_write ? 1 : 0) |
                          VIRGL_DSA_S0_DEPTH_FUNC(PIPE_FUNC_LESS))
                       : (VIRGL_DSA_S0_DEPTH_ENABLE(1) |
                          VIRGL_DSA_S0_DEPTH_FUNC(PIPE_FUNC_ALWAYS)))
                  | VIRGL_DSA_S0_ALPHA_ENABLE(1)
                  | VIRGL_DSA_S0_ALPHA_FUNC(wv->alpha_func);
        if (!wv->dsa_alpha_h || s0 != wv->dsa_alpha_s0 ||
            wv->alpha_ref != wv->dsa_alpha_ref) {
            uint32 nh = handle_flip(wv->dsa_alpha_h, W3D_HANDLE_DSA_ALPHA,
                                    W3D_HANDLE_DSA_ALPHA + 2);
            virgl_cmd_create_dsa(cbuf, nh, s0, 0, 0, wv->alpha_ref);
            if (wv->dsa_alpha_h)
                virgl_cmd_destroy_object(cbuf, VIRGL_OBJECT_DSA, wv->dsa_alpha_h);
            wv->dsa_alpha_h  = nh;
            wv->dsa_alpha_s0 = s0;
            wv->dsa_alpha_ref = wv->alpha_ref;
        }
        virgl_cmd_bind_object(cbuf, VIRGL_OBJECT_DSA, wv->dsa_alpha_h);
    } else {
        virgl_cmd_bind_object(cbuf, VIRGL_OBJECT_DSA,
            wv->depth_test ? (wv->depth_write ? 300 : 301) : 302);
    }
}

/* Resolve the sampler for a textured draw: the texture's private sampler
 * (rebuilt lazily HERE, inside the draw's cbuf, when W3D_SetFilter /
 * W3D_SetWrapMode changed it) or the chip's shared linear sampler when the
 * app never touched filter/wrap (preserves the proven default). */
static uint32 w3d_wrapmode(uint32 w)
{
    switch (w) {
    case W3D_REPEAT:     return PIPE_TEX_WRAP_REPEAT;
    case W3D_CLAMP_LAST: return PIPE_TEX_WRAP_CLAMP_TO_EDGE;
    case 3:              return PIPE_TEX_WRAP_CLAMP_TO_BORDER; /* W3D_CLAMP_BORDER_COLOR */
    default:             return PIPE_TEX_WRAP_CLAMP_TO_EDGE;   /* chip default */
    }
}
static uint32 tex_sampler(struct VirglCmdBuf *cbuf, struct W3DVirgl *wv,
                          struct W3DTexInfo *ti)
{
    if (ti->sampler_dirty) {
        /* image filter: any W3D_NEAREST* variant -> NEAREST, else LINEAR
         * (mip variants collapse -- no mipmaps are uploaded yet) */
        uint32 fmin = (ti->filter_min == W3D_NEAREST ||
                       ti->filter_min == W3D_NEAREST_MIP_NEAREST ||
                       ti->filter_min == W3D_NEAREST_MIP_LINEAR ||
                       ti->filter_min == W3D_ANISOTROPIC_NEAREST)
                      ? PIPE_TEX_FILTER_NEAREST : PIPE_TEX_FILTER_LINEAR;
        uint32 fmag = (ti->filter_mag == W3D_NEAREST)
                      ? PIPE_TEX_FILTER_NEAREST : PIPE_TEX_FILTER_LINEAR;
        uint32 ws = w3d_wrapmode(ti->wrap_s), wt = w3d_wrapmode(ti->wrap_t);
        uint32 s0 = VIRGL_SAMPLER_S0_WRAP_S(ws)
                  | VIRGL_SAMPLER_S0_WRAP_T(wt)
                  | VIRGL_SAMPLER_S0_WRAP_R(ws)
                  | VIRGL_SAMPLER_S0_MIN_IMG_FILTER(fmin)
                  | VIRGL_SAMPLER_S0_MIN_MIP_FILTER(PIPE_TEX_MIPFILTER_NONE)
                  | VIRGL_SAMPLER_S0_MAG_IMG_FILTER(fmag);
        uint32 nh = wv->sampler_next++;
        virgl_cmd_create_sampler_state(cbuf, nh, s0, 0.0f, 0.0f, 0.0f,
                                       ti->border[0], ti->border[1],
                                       ti->border[2], ti->border[3]);
        if (ti->sampler)
            virgl_cmd_destroy_object(cbuf, VIRGL_OBJECT_SAMPLER_STATE,
                                     ti->sampler);
        ti->sampler = nh;
        ti->sampler_dirty = 0;
    }
    return ti->sampler ? ti->sampler : wv->info.sampler_linear;
}

/* Bind the W3D rasterizer: clone of the chip's base state (depth clip, fill,
 * SCISSOR) plus backface cull + front-face winding.  Bound on EVERY draw so
 * turning cull off deterministically restores a no-cull object. */
static void bind_rasterizer(struct VirglCmdBuf *cbuf, struct W3DVirgl *wv)
{
    uint32 s0 = VIRGL_RS_S0_DEPTH_CLIP(1)
              | VIRGL_RS_S0_CULL_FACE(wv->cull_on ? PIPE_FACE_BACK
                                                  : PIPE_FACE_NONE)
              | VIRGL_RS_S0_FILL_FRONT(PIPE_POLYGON_MODE_FILL)
              | VIRGL_RS_S0_FILL_BACK(PIPE_POLYGON_MODE_FILL)
              | VIRGL_RS_S0_SCISSOR(1)
              | VIRGL_RS_S0_FRONT_CCW(wv->front_ccw ? 1 : 0);
    if (!wv->rast_h || s0 != wv->rast_s0) {
        uint32 nh = handle_flip(wv->rast_h, W3D_HANDLE_RAST,
                                W3D_HANDLE_RAST + 2);
        virgl_cmd_create_rasterizer(cbuf, nh, s0, 1.0f, 0, 0,
                                    1.0f, 0.0f, 0.0f, 0.0f);
        if (wv->rast_h)
            virgl_cmd_destroy_object(cbuf, VIRGL_OBJECT_RASTERIZER, wv->rast_h);
        wv->rast_h  = nh;
        wv->rast_s0 = s0;
    }
    virgl_cmd_bind_object(cbuf, VIRGL_OBJECT_RASTERIZER, wv->rast_h);
}

static void bind_rt_framebuffer(struct VirglCmdBuf *cbuf, struct W3DVirgl *wv)
{
    uint32 surf = wv->rt_surface[wv->draw_idx];
    float hw = (float)wv->fb_w * 0.5f, hh = (float)wv->fb_h * 0.5f;
    /* colour surface + depth (zsurf) so the depth test has a buffer */
    virgl_cmd_set_framebuffer_state(cbuf, 1, wv->zsurf, &surf);
    /* Re-assert the viewport at the RT size EVERY draw.  We share the chip's virgl
     * context, whose viewport is the scanout size (e.g. 1280x800); without this the
     * 640x480 cow is scaled ~2x and pushed bottom-right.  POSITIVE scale_y -- the
     * present blit is not Y-flipped, so this keeps the cow upright. */
    virgl_cmd_set_viewport(cbuf, 0, hw, hh, 0.5f, hw, hh, 0.5f);
    /* scissor >= RT so it never clips (the chip left it at scanout size) */
    virgl_cmd_set_scissor_state(cbuf, 0, 0, 0, wv->fb_w, wv->fb_h);
    /* DSA (depth + alpha test), blend (factors + colour mask), rasterizer
     * (cull + winding) -- each rebuilt on demand via twin handles */
    bind_dsa(cbuf, wv);
    bind_blend(cbuf, wv);
    bind_rasterizer(cbuf, wv);
}

/* Frame boundary (called from ClearBuffers/ClearDrawRegion): if geometry was
 * drawn since the last clear, the current back buffer now holds a COMPLETE
 * frame -- register it as the overlay and swap buffers.  Then record the clear
 * colour so the next draw clears the new (now-back) buffer. */
static void frame_clear(struct W3DVirgl *wv, uint32 argb)
{
    /* M3: single render target presented into the app bitmap on FlushFrame --
     * the clear is deferred and emitted with the next draw (one atomic submit). */
    wv->drawn_since_clear = FALSE;
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
    /* BACKEND model: present the RT into the app bitmap right away -- the cow's
     * test phase never calls FlushFrame, so without this the window stays grey.
     * (Per-draw present is heavy under TCG; acceptable for first-light.) */
    DW3D("draw_packed: %lu verts prim=%lu -> present bm=%p\n",
         (unsigned long)nverts, (unsigned long)pipe_prim, (void *)wv->bm);
    if (wv->bm) frame_present(wv, wv->bm);
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

    /* Bring up the transport on first use. Phase 3.2b-2: routed through
     * gpu.library's 'virtio-gpu' backend (VGB_OP_V3DCALL) instead of the
     * chip's private "v3d" interface -- the adapter above presents the
     * same V3DIFace surface, so the render code is unchanged. */
    if (!g_IV3D) {
        if (!g_GpuBase) {
            g_GpuBase = IExec->OpenLibrary("gpu.library", 53);
            if (g_GpuBase)
                g_IGpu = (struct GpuIFace *)
                    IExec->GetInterface(g_GpuBase, "main", 1, NULL);
        }
        if (g_IGpu && g_gpuVid < 0) {
            uint32 i;
            for (i = 0; i < 4; i++) {
                CONST_STRPTR name = NULL;
                g_IGpu->GPU_GetAttrsA(GPU_TAGS(
                    { GPUATTR_BackendIndex, i },
                    { GPUATTR_BackendName,  (uint32)&name }));
                if (name && name[0] == 'v' && name[1] == 'i') {
                    g_gpuVid = (int32)i;
                    break;
                }
            }
        }
        if (g_gpuVid >= 0)
            g_IV3D = &v3dgpu_iface;
    }
    if (!g_IV3D) {
        DW3D("CreateContext: gpu.library 'virtio-gpu' backend unavailable\n");
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

    /* Render at the W3D_CC_BITMAP's dimensions (a windowed app draws into its
     * own off-screen bitmap, e.g. 640x480), NOT the chip's full scanout size.
     * Falls back to the scanout dims if the bitmap can't be queried. */
    wv->fb_w = wv->info.fb_width;
    wv->fb_h = wv->info.fb_height;
    {
        uint32 bw = 0, bh = 0;
        if (bm && bitmap_geometry(bm, &bw, &bh, NULL) && bw && bh) {
            wv->fb_w = bw; wv->fb_h = bh;
        }
    }
    DW3D("CreateContext: drawregion bitmap %lux%lu\n",
         (unsigned long)wv->fb_w, (unsigned long)wv->fb_h);

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
    wv->texenv_mode = W3D_REPLACE;   /* REPLACE = the current 2-attr path (R8) */
    wv->texenv_mode_logged = 0;      /* force one census line on first draw */

    /* Shared depth buffer + a depth-test DSA (LESS, write enabled) so the cow
     * surfaces occlude correctly regardless of triangle draw order. */
    wv->depth_test = TRUE; wv->depth_write = TRUE;
    if (g_IV3D->AllocDepthBuffer(g_IV3D, wv->info.token, wv->fb_w, wv->fb_h,
                                 &wv->zres, &wv->zsurf)) {
        uint32 dw[48]; struct VirglCmdBuf dcb;
        wv->dsa_handle = 300;       /* warp3d object handle range (>= chip's) */
        virgl_cmd_init(&dcb, dw, 48);
        /* 300 = test+write (LESS), 301 = test, no write, 302 = depth OFF.
         * bind_rt_framebuffer picks one per draw from depth_test/depth_write. */
        virgl_cmd_create_dsa(&dcb, 300,
            VIRGL_DSA_S0_DEPTH_ENABLE(1) | VIRGL_DSA_S0_DEPTH_WRITEMASK(1) |
            VIRGL_DSA_S0_DEPTH_FUNC(PIPE_FUNC_LESS), 0, 0, 0.0f);
        virgl_cmd_create_dsa(&dcb, 301,
            VIRGL_DSA_S0_DEPTH_ENABLE(1) | VIRGL_DSA_S0_DEPTH_WRITEMASK(0) |
            VIRGL_DSA_S0_DEPTH_FUNC(PIPE_FUNC_LESS), 0, 0, 0.0f);
        /* 302 = "depth off": well-formed as test-ENABLED + func ALWAYS + no write
         * (a bare DEPTH_ENABLE(0)/s0=0 left the Cosmos overlay invisible -- some
         * virglrenderer paths mishandle the all-zero DSA).  ALWAYS passes every
         * fragment, WRITEMASK(0) leaves the depth buffer intact. */
        virgl_cmd_create_dsa(&dcb, 302,
            VIRGL_DSA_S0_DEPTH_ENABLE(1) | VIRGL_DSA_S0_DEPTH_WRITEMASK(0) |
            VIRGL_DSA_S0_DEPTH_FUNC(PIPE_FUNC_ALWAYS), 0, 0, 0.0f);
        g_IV3D->Submit(g_IV3D, wv->info.token, wv->info.ctx_id, dcb.buf, dcb.dwords);
    }

    /* Blend objects: opaque (default) + an app-blend object built from the
     * W3D_SetBlendMode factors (init ONE/ONE = additive, the cow's default).
     * bind_blend() selects between them per draw via W3D_BLENDING. */
    {
        uint32 bw2[64]; struct VirglCmdBuf bcb;   /* 2x create_blend = 24 words */
        wv->src_blend = W3D_ONE; wv->dst_blend = W3D_ONE;
        wv->blend_on = FALSE; wv->blend_funcs_dirty = FALSE;
        /* 8b dynamic-state defaults: full colour mask, CCW front faces, no
         * private objects yet (twin-handle recreate builds them on demand) */
        wv->color_mask = 0xF;
        wv->blend_mask_dirty = FALSE;
        wv->blend_opaque_h = W3D_HANDLE_BLEND_OPAQUE;
        wv->blend_func_h   = W3D_HANDLE_BLEND_FUNC;
        wv->alpha_func = 0; wv->alpha_ref = 0.0f; wv->alpha_on = FALSE;
        wv->dsa_alpha_h = 0;
        wv->cull_on = FALSE; wv->front_ccw = TRUE;
        wv->rast_h = 0;
        wv->sampler_next = 340;
        virgl_cmd_init(&bcb, bw2, 64);
        virgl_cmd_create_blend(&bcb, W3D_HANDLE_BLEND_OPAQUE, 0, VIRGL_BLEND_RT_OPAQUE);
        virgl_cmd_create_blend(&bcb, W3D_HANDLE_BLEND_FUNC, 0,
            VIRGL_BLEND_RT_BLEND_ENABLE(1)
            | VIRGL_BLEND_RT_RGB_FUNC(PIPE_BLEND_ADD)
            | VIRGL_BLEND_RT_RGB_SRC_FACTOR(PIPE_BLENDFACTOR_ONE)
            | VIRGL_BLEND_RT_RGB_DST_FACTOR(PIPE_BLENDFACTOR_ONE)
            | VIRGL_BLEND_RT_ALPHA_FUNC(PIPE_BLEND_ADD)
            | VIRGL_BLEND_RT_ALPHA_SRC_FACTOR(PIPE_BLENDFACTOR_ONE)
            | VIRGL_BLEND_RT_ALPHA_DST_FACTOR(PIPE_BLENDFACTOR_ONE)
            | VIRGL_BLEND_RT_COLORMASK(0xF));
        g_IV3D->Submit(g_IV3D, wv->info.token, wv->info.ctx_id, bcb.buf, bcb.dwords);
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
    /* M3: no scanout overlay -- warp3d renders into rt_res[0] and presents it
     * straight into the app's W3D_CC_BITMAP on FlushFrame; the app blits that
     * bitmap into its own window. */
    wv->draw_idx = 0;

    /* Lower the caller's task priority: the demo renders flat-out and warp3d
     * runs in its task, so at low priority the desktop/input/flush tasks always
     * preempt it -> cursor stays responsive under single-core TCG. */
    {
        struct Task *me = IExec->FindTask(NULL);
        if (me) {
            wv->saved_pri = (LONG)IExec->SetTaskPri(me, -5);
            wv->pri_lowered = TRUE;
        }
    }

    /* timer.device for the WaitIdle CPU-yield (created in the caller's task). */
    wv->timer_mp = IExec->AllocSysObjectTags(ASOT_PORT, TAG_END);
    if (wv->timer_mp) {
        wv->timer_io = IExec->AllocSysObjectTags(ASOT_IOREQUEST,
            ASOIOR_Size, sizeof(struct TimeRequest),
            ASOIOR_ReplyPort, wv->timer_mp, TAG_END);
        if (wv->timer_io &&
            IExec->OpenDevice("timer.device", UNIT_MICROHZ,
                              (struct IORequest *)wv->timer_io, 0) != 0) {
            IExec->FreeSysObject(ASOT_IOREQUEST, wv->timer_io);
            wv->timer_io = NULL;
        }
    }

    wv->bm          = bm;           /* present target (backend model) */
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
        if (wv->timer_io) {
            IExec->CloseDevice((struct IORequest *)wv->timer_io);
            IExec->FreeSysObject(ASOT_IOREQUEST, wv->timer_io);
        }
        if (wv->timer_mp) IExec->FreeSysObject(ASOT_PORT, wv->timer_mp);
        if (wv->pri_lowered) {
            struct Task *me = IExec->FindTask(NULL);
            if (me) IExec->SetTaskPri(me, wv->saved_pri);
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
    BOOL en;
    (void)Self;
    if (!ctx || !ctx->driver) return W3D_ILLEGALINPUT;
    wv = ctx->driver;
    /* action is W3D_ENABLE(1) or W3D_DISABLE(2) -- NOT a 0/1 bool, so the old
     * `if (action)` treated DISABLE(2) as enable (blending/depth stuck on). */
    /* NOTE: the FE's W3D_SetState does NOT pass the enable/disable to this backend
     * vector -- it hands us a per-state selector (e.g. 0x29 for ZBUFFER) and records
     * the actual bit in ctx+0x1c itself.  So we do NOT derive depth/blend here (that
     * is read from ctx+0x1c per-draw in w3d_DrawElements) and must NOT write ctx->state
     * (would clobber the FE's mirror).  Keep a local wv->state echo only. */
    en = (action == W3D_ENABLE);
    if (en) wv->state |= state; else wv->state &= ~state;
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
void w3d_WaitIdle(struct Warp3DIFace *Self, W3D_Context *ctx)
{
    struct W3DVirgl *wv;
    (void)Self;
    /* Yield the CPU briefly (the GPU work is already submitted/synchronous).
     * The demo runs uncapped and calls WaitIdle several times per frame; this
     * micro-sleep lets the desktop/input/flush tasks run under single-core TCG
     * so the cursor doesn't freeze. */
    if (!ctx || !ctx->driver) return;
    wv = ctx->driver;
    if (wv->timer_io) {
        struct TimeRequest *tr = (struct TimeRequest *)wv->timer_io;
        tr->Request.io_Command = TR_ADDREQUEST;
        tr->Time.Seconds      = 0;
        tr->Time.Microseconds = 300;
        IExec->DoIO((struct IORequest *)tr);
    }
}
uint32 w3d_CheckIdle(struct Warp3DIFace *Self, W3D_Context *ctx)    { (void)Self; (void)ctx; return TRUE; }

uint32 w3d_SetBlendMode(struct Warp3DIFace *Self, W3D_Context *ctx, uint32 s, uint32 d)
{
    struct W3DVirgl *wv;
    (void)Self;
    if (!ctx || !ctx->driver) return W3D_ILLEGALINPUT;
    wv = ctx->driver;
    if (wv->src_blend != s || wv->dst_blend != d) {
        wv->src_blend = s; wv->dst_blend = d;
        wv->blend_funcs_dirty = TRUE;   /* rebuild the app-blend object */
    }
    return W3D_SUCCESS;
}

/* W3D_A_* (1..8) -> PIPE_FUNC_* for the alpha-test DSA */
static uint32 w3d_alphafunc(uint32 mode)
{
    switch (mode) {
    case W3D_A_NEVER:    return PIPE_FUNC_NEVER;
    case W3D_A_LESS:     return PIPE_FUNC_LESS;
    case W3D_A_GEQUAL:   return PIPE_FUNC_GEQUAL;
    case W3D_A_LEQUAL:   return PIPE_FUNC_LEQUAL;
    case W3D_A_GREATER:  return PIPE_FUNC_GREATER;
    case W3D_A_NOTEQUAL: return PIPE_FUNC_NOTEQUAL;
    case W3D_A_EQUAL:    return PIPE_FUNC_EQUAL;
    case W3D_A_ALWAYS:   return PIPE_FUNC_ALWAYS;
    default:             return PIPE_FUNC_ALWAYS;
    }
}

uint32 w3d_SetAlphaMode(struct Warp3DIFace *Self, W3D_Context *ctx,
                        uint32 mode, W3D_Float *refval)
{
    struct W3DVirgl *wv;
    (void)Self;
    if (!ctx || !ctx->driver) return W3D_ILLEGALINPUT;
    wv = ctx->driver;
    wv->alpha_func = w3d_alphafunc(mode);
    wv->alpha_ref  = refval ? (float)*refval : 0.0f;
    DW3D("SetAlphaMode: mode=%lu ref*1000=%ld\n", (unsigned long)mode,
         (long)(wv->alpha_ref * 1000.0f));
    return W3D_SUCCESS;
}

uint32 w3d_SetColorMask(struct Warp3DIFace *Self, W3D_Context *ctx,
                        uint32 r, uint32 g, uint32 b, uint32 a)
{
    struct W3DVirgl *wv;
    uint32 mask;
    (void)Self;
    if (!ctx || !ctx->driver) return W3D_ILLEGALINPUT;
    wv = ctx->driver;
    /* pipe colormask: bit0=R bit1=G bit2=B bit3=A */
    mask = (r ? 1u : 0) | (g ? 2u : 0) | (b ? 4u : 0) | (a ? 8u : 0);
    if (mask != wv->color_mask) {
        wv->color_mask = mask;
        wv->blend_mask_dirty  = TRUE;   /* rebuild BOTH blend objects */
        wv->blend_funcs_dirty = TRUE;
    }
    DW3D("SetColorMask: mask=0x%lx\n", (unsigned long)mask);
    return W3D_SUCCESS;
}

uint32 w3d_SetFrontFace(struct Warp3DIFace *Self, W3D_Context *ctx, uint32 dir)
{
    struct W3DVirgl *wv;
    (void)Self;
    if (!ctx || !ctx->driver) return W3D_ILLEGALINPUT;
    wv = ctx->driver;
    wv->front_ccw = (dir == W3D_CCW);   /* rasterizer rebuilds on next draw */
    DW3D("SetFrontFace: %s\n", wv->front_ccw ? "CCW" : "CW");
    return W3D_SUCCESS;
}

/* Per-texture filter/wrap recorders (FE slots 30/32; sampler object is
 * rebuilt lazily inside the next draw's command buffer). */
uint32 w3d_SetTexFilter(W3D_Context *ctx, W3D_Texture *tex,
                        uint32 fmin, uint32 fmag)
{
    struct W3DTexInfo *ti;
    (void)ctx;
    if (!tex || !tex->driver) return W3D_SUCCESS;  /* pre-realize call: defaults */
    ti = tex->driver;
    if (ti->magic != W3DTEX_MAGIC) return W3D_SUCCESS;
    if (ti->filter_min != fmin || ti->filter_mag != fmag) {
        ti->filter_min = fmin; ti->filter_mag = fmag;
        ti->sampler_dirty = 1;
    }
    DW3D("SetFilter: tex=%p min=%lu mag=%lu\n", (void *)tex,
         (unsigned long)fmin, (unsigned long)fmag);
    return W3D_SUCCESS;
}

uint32 w3d_SetTexWrap(W3D_Context *ctx, W3D_Texture *tex,
                      uint32 mode_s, uint32 mode_t, W3D_Color *border)
{
    struct W3DTexInfo *ti;
    (void)ctx;
    if (!tex || !tex->driver) return W3D_SUCCESS;
    ti = tex->driver;
    if (ti->magic != W3DTEX_MAGIC) return W3D_SUCCESS;
    if (ti->wrap_s != mode_s || ti->wrap_t != mode_t || border) {
        ti->wrap_s = mode_s; ti->wrap_t = mode_t;
        if (border) {
            ti->border[0] = border->r; ti->border[1] = border->g;
            ti->border[2] = border->b; ti->border[3] = border->a;
        }
        ti->sampler_dirty = 1;
    }
    DW3D("SetWrapMode: tex=%p s=%lu t=%lu\n", (void *)tex,
         (unsigned long)mode_s, (unsigned long)mode_t);
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
/* Immediate-mode triangle ops -- the stock Warp3D FE's CreateContext test    */
/* phase (DrawZtests/CheckBlendModes) draws via these, NOT DrawElements.       */
/* The FE preserves ctx in r4, so each gets (Self, ctx, <struct*>).  M1: cap    */
/* at 8 verts (the shared 256-byte vbuf) -- enough for the test quads/fans.     */
/* ----------------------------------------------------------------------- */
/* W3D_DrawTriFan / W3D_DrawTriStrip: W3D_Triangles { int vertexcount;          */
/*   W3D_Vertex *v; ... } -- an inline vertex array.                            */
static uint32 w3d_draw_varray(struct W3DVirgl *wv, const W3D_Vertex *v,
                              int count, uint32 pipe_prim)
{
    float verts[64];          /* 8 verts * 8 floats */
    int i;
    if (!v || count < 3) return W3D_ILLEGALINPUT;
    if (count > 8) {
        DW3D("draw_varray: count %ld > 8 (M1 vbuf cap) -- clamped\n", (long)count);
        count = 8;
    }
    for (i = 0; i < count; i++)
        pack_vertex(&verts[i * 8], &v[i], (float)wv->fb_w, (float)wv->fb_h);
    return draw_packed(wv, verts, (uint32)count, pipe_prim);
}

uint32 w3d_DrawTriFan(struct Warp3DIFace *Self, W3D_Context *ctx, W3D_Triangles *t)
{
    (void)Self;
    if (!ctx || !ctx->driver || !t) return W3D_ILLEGALINPUT;
    DW3D("DrawTriFan: %ld verts\n", (long)t->vertexcount);
    return w3d_draw_varray(ctx->driver, t->v, t->vertexcount, PIPE_PRIM_TRIANGLE_FAN);
}
uint32 w3d_DrawTriStrip(struct Warp3DIFace *Self, W3D_Context *ctx, W3D_Triangles *t)
{
    (void)Self;
    if (!ctx || !ctx->driver || !t) return W3D_ILLEGALINPUT;
    DW3D("DrawTriStrip: %ld verts\n", (long)t->vertexcount);
    return w3d_draw_varray(ctx->driver, t->v, t->vertexcount, PIPE_PRIM_TRIANGLE_STRIP);
}

/* W3D_DrawTriangleV: W3D_TriangleV { W3D_Vertex *v1,*v2,*v3; ... } -- pointers. */
uint32 w3d_DrawTriangleV(struct Warp3DIFace *Self, W3D_Context *ctx, W3D_TriangleV *t)
{
    struct W3DVirgl *wv;
    float verts[24];
    (void)Self;
    if (!ctx || !ctx->driver || !t || !t->v1 || !t->v2 || !t->v3)
        return W3D_ILLEGALINPUT;
    wv = ctx->driver;
    DW3D("DrawTriangleV\n");
    pack_vertex(&verts[0],  t->v1, (float)wv->fb_w, (float)wv->fb_h);
    pack_vertex(&verts[8],  t->v2, (float)wv->fb_w, (float)wv->fb_h);
    pack_vertex(&verts[16], t->v3, (float)wv->fb_w, (float)wv->fb_h);
    return draw_packed(wv, verts, 3, PIPE_PRIM_TRIANGLES);
}

/* W3D_DrawTriFanV / W3D_DrawTriStripV: W3D_TrianglesV { int vertexcount;        */
/*   W3D_Vertex **v; ... } -- an array of vertex POINTERS.                      */
static uint32 w3d_draw_varrayV(struct W3DVirgl *wv, W3D_Vertex **v,
                               int count, uint32 pipe_prim)
{
    float verts[64];
    int i;
    if (!v || count < 3) return W3D_ILLEGALINPUT;
    if (count > 8) count = 8;
    for (i = 0; i < count; i++) {
        if (!v[i]) return W3D_ILLEGALINPUT;
        pack_vertex(&verts[i * 8], v[i], (float)wv->fb_w, (float)wv->fb_h);
    }
    return draw_packed(wv, verts, (uint32)count, pipe_prim);
}
uint32 w3d_DrawTriFanV(struct Warp3DIFace *Self, W3D_Context *ctx, W3D_TrianglesV *t)
{
    (void)Self;
    if (!ctx || !ctx->driver || !t) return W3D_ILLEGALINPUT;
    return w3d_draw_varrayV(ctx->driver, t->v, t->vertexcount, PIPE_PRIM_TRIANGLE_FAN);
}
uint32 w3d_DrawTriStripV(struct Warp3DIFace *Self, W3D_Context *ctx, W3D_TrianglesV *t)
{
    (void)Self;
    if (!ctx || !ctx->driver || !t) return W3D_ILLEGALINPUT;
    return w3d_draw_varrayV(ctx->driver, t->v, t->vertexcount, PIPE_PRIM_TRIANGLE_STRIP);
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
    struct W3DTexInfo *ti;
    struct W3DVirgl *wv;
    APTR   image;
    uint32 w, h;
    (void)Self;

    if (!ctx || !ctx->driver) { if (error) *error = W3D_ILLEGALINPUT; return NULL; }
    wv = ctx->driver;

    image = (APTR)w3d_tagdata(tags, W3D_ATO_IMAGE, 0);
    w     = w3d_tagdata(tags, W3D_ATO_WIDTH,  0);
    h     = w3d_tagdata(tags, W3D_ATO_HEIGHT, 0);
    DW3D("AllocTexObj: tags=%p image=%p w=%lu h=%lu\n", (void *)tags,
         image, (unsigned long)w, (unsigned long)h);
    if (!image || !w || !h) { if (error) *error = W3D_ILLEGALINPUT; return NULL; }

    tex = IExec->AllocVecTags(sizeof(W3D_Texture),
                              AVT_Type, MEMF_PRIVATE, AVT_ClearWithValue, 0, TAG_DONE);
    ti  = IExec->AllocVecTags(sizeof(struct W3DTexInfo),
                              AVT_Type, MEMF_PRIVATE, AVT_ClearWithValue, 0, TAG_DONE);
    if (!tex || !ti) {
        if (tex) IExec->FreeVec(tex);
        if (ti)  IExec->FreeVec(ti);
        if (error) *error = W3D_NOMEMORY;
        return NULL;
    }

    /* Upload as R8G8B8A8 (the cow's textures are 32bpp RGBA RAW). */
    if (!g_IV3D->CreateTexture(g_IV3D, wv->info.token, w, h, image, w * 4,
                               &ti->view, &ti->res)) {
        DW3D("AllocTexObj: CreateTexture failed %lux%lu\n",
             (unsigned long)w, (unsigned long)h);
        IExec->FreeVec(tex); IExec->FreeVec(ti);
        if (error) *error = W3D_NOMEMORY;
        return NULL;
    }
    ti->w = w; ti->h = h;
    ti->magic = W3DTEX_MAGIC;
    tex->driver    = ti;
    tex->texwidth  = (int)w;
    tex->texheight = (int)h;
    if (error) *error = W3D_SUCCESS;
    DW3D("AllocTexObj: OK %lux%lu res=%lu view=%lu\n",
         (unsigned long)w, (unsigned long)h,
         (unsigned long)ti->res, (unsigned long)ti->view);
    return tex;
}

W3D_Texture *w3d_AllocTexObjTags(struct Warp3DIFace *Self, W3D_Context *ctx,
                                 uint32 *error, ...)
{
    /* AmigaOS4 clib2 callers (e.g. the CoW3D demo) pass tag varargs on the
     * stack, not in r6-r10.  The PPC SysV va_list's overflow_arg_area field
     * (offset 4 in __va_list_tag) points at the first stack vararg, i.e. the
     * head of the contiguous tag list; the register-save area holds garbage.
     * Read the tag list straight from there -- robust where va_arg, which
     * consults the register-save area first, mis-reads it. */
    va_list ap;
    struct TagItem *tags;
    va_start(ap, error);
    tags = (struct TagItem *)(*(void **)((unsigned char *)ap + 4));
    va_end(ap);
    return w3d_AllocTexObj(Self, ctx, error, tags);
}

/* The stock FE OWNS the W3D_Texture (it allocated it in W3D_AllocTexObj and
 * frees it itself after the backend slot returns) -- we free ONLY our driver
 * data + GPU resources, and only when the magic proves ti is really ours
 * (slot arg layouts are RE-derived; a mis-decoded call must be a logged
 * no-op, not a FreeVec of garbage: that was a suite-found DSI + a
 * RESOURCE_UNREF storm with pointer-valued ids). */
void w3d_FreeTexObj(struct Warp3DIFace *Self, W3D_Context *ctx, W3D_Texture *tex)
{
    struct W3DVirgl *wv;
    struct W3DTexInfo *ti;
    (void)Self;
    if (!tex) return;
    ti = tex->driver;
    if (!ti || ti->magic != W3DTEX_MAGIC) {
        DW3D("FreeTexObj: tex=%p driver=%p NO MAGIC -- ignored\n",
             (void *)tex, (void *)ti);
        return;
    }
    if (ctx && ctx->driver) {
        wv = ctx->driver;
        if (wv->cur_tex == tex) wv->cur_tex = NULL;
        if (g_IV3D) {
            if (ti->sampler) {   /* private filter/wrap sampler object */
                uint32 dw[8]; struct VirglCmdBuf dcb;
                virgl_cmd_init(&dcb, dw, 8);
                virgl_cmd_destroy_object(&dcb, VIRGL_OBJECT_SAMPLER_STATE,
                                         ti->sampler);
                g_IV3D->Submit(g_IV3D, wv->info.token, wv->info.ctx_id,
                               dcb.buf, dcb.dwords);
            }
            g_IV3D->FreeTexture(g_IV3D, wv->info.token, ti->res, ti->view);
        }
    }
    ti->magic = 0;
    tex->driver = NULL;
    IExec->FreeVec(ti);
}

uint32 w3d_BindTexture(struct Warp3DIFace *Self, W3D_Context *ctx,
                       uint32 tmu, W3D_Texture *tex)
{
    struct W3DVirgl *wv;
    (void)Self;
    if (!ctx || !ctx->driver) return W3D_ILLEGALINPUT;
    wv = ctx->driver;
    if (tmu == 0) wv->cur_tex = tex;   /* only TMU0 (single-texture) */
    return W3D_SUCCESS;
}

/* Realize a FE-built W3D_Texture.  The stock Warp3D.library FE allocates+fills
 * the W3D_Texture itself (parses the ATO tags into texsource/texwidth/texheight)
 * then calls the backend's off-0xd4 slot to UPLOAD it -- it never calls a tag-
 * taking backend create.  So this reads the struct fields (vs w3d_AllocTexObj
 * which reads tags) and uploads as R8G8B8A8, stashing {res,view} in tex->driver
 * (which w3d_BindTexture + draw_elements_chunk read).  Returns W3D_SUCCESS(0). */
uint32 w3d_RealizeTexture(struct Warp3DIFace *Self, W3D_Context *ctx, W3D_Texture *tex)
{
    struct W3DTexInfo *ti;
    struct W3DVirgl   *wv;
    APTR   image;
    uint32 w, h;
    (void)Self;

    if (!ctx || !ctx->driver || !tex) return W3D_ILLEGALINPUT;
    wv = ctx->driver;
    if (tex->driver) return W3D_SUCCESS;        /* already realized -- idempotent */

    image = tex->texsource;
    w     = (uint32)tex->texwidth;
    h     = (uint32)tex->texheight;
    DW3D("RealizeTexture: tex=%p image=%p %lux%lu\n", (void *)tex, image,
         (unsigned long)w, (unsigned long)h);
    if (!image || !w || !h) return W3D_ILLEGALINPUT;

    ti = IExec->AllocVecTags(sizeof(struct W3DTexInfo),
                             AVT_Type, MEMF_PRIVATE, AVT_ClearWithValue, 0, TAG_DONE);
    if (!ti) return W3D_NOMEMORY;

    /* Cow textures are 32bpp RGBA RAW -> R8G8B8A8 (chip GP32 swap handles BE/LE). */
    if (!g_IV3D->CreateTexture(g_IV3D, wv->info.token, w, h, image, w * 4,
                               &ti->view, &ti->res)) {
        DW3D("RealizeTexture: CreateTexture failed %lux%lu\n",
             (unsigned long)w, (unsigned long)h);
        IExec->FreeVec(ti);
        return W3D_NOMEMORY;
    }
    ti->w = w; ti->h = h;
    ti->magic = W3DTEX_MAGIC;
    tex->driver = ti;
    DW3D("RealizeTexture: OK %lux%lu res=%lu view=%lu\n",
         (unsigned long)w, (unsigned long)h,
         (unsigned long)ti->res, (unsigned long)ti->view);
    return W3D_SUCCESS;
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
    DW3D("InterleavedArray: stride=%ld fmt=%08lx\n", (long)stride, (unsigned long)format);

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
    if (format & W3D_VFORMAT_TCOORD_0)   { wv->ia_has_tcoord = TRUE; wv->ia_tcoord_off = off; off += 12; } /* u,v,w = 3 floats (SDK), not 8 */

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
    uint32 i, num_floats;
    /* Textured draw if a texture is bound and the array carries texcoords. */
    struct W3DTexInfo *ti = (wv->cur_tex && wv->cur_tex->driver && wv->ia_has_tcoord)
                            ? (struct W3DTexInfo *)wv->cur_tex->driver : NULL;
    /* WIDE combine path: textured + non-REPLACE + the 3-attr objects exist + the
     * array carries colour.  Else the EXACT original 8-float/stride-32 path runs
     * (REPLACE + untextured = R8-safe, byte-identical). */
    BOOL wide = (ti && wv->texenv_mode != W3D_REPLACE && wv->ia_has_color &&
                 wv->info.vs3_handle && wv->info.ve3_handle &&
                 ((wv->texenv_mode == W3D_MODULATE && wv->info.fs_modulate_handle) ||
                  (wv->texenv_mode == W3D_DECAL    && wv->info.fs_decal_handle)    ||
                  (wv->texenv_mode == W3D_BLEND    && wv->info.fs_blend_handle)));
    num_floats = nverts * (wide ? 12 : 8);

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

    if (wide) {
        /* MODULATE/DECAL/BLEND: 3-attr VS (pos+tc+colour) + the mode's FS. */
        uint32 fs = (wv->texenv_mode == W3D_MODULATE) ? wv->info.fs_modulate_handle
                  : (wv->texenv_mode == W3D_DECAL)    ? wv->info.fs_decal_handle
                  :                                     wv->info.fs_blend_handle;
        uint32 samp = tex_sampler(&cbuf, wv, ti);
        virgl_cmd_bind_object(&cbuf, VIRGL_OBJECT_VERTEX_ELEMENTS, wv->info.ve3_handle);
        virgl_cmd_bind_shader(&cbuf, PIPE_SHADER_VERTEX, wv->info.vs3_handle);
        virgl_cmd_bind_shader(&cbuf, PIPE_SHADER_FRAGMENT, fs);
        virgl_cmd_bind_sampler_states(&cbuf, PIPE_SHADER_FRAGMENT, 0, 1, &samp);
        virgl_cmd_set_sampler_views(&cbuf, PIPE_SHADER_FRAGMENT, 0, 1, &ti->view);
        if (wv->texenv_mode == W3D_BLEND)
            virgl_cmd_set_constant_buffer(&cbuf, PIPE_SHADER_FRAGMENT, 0,
                                          wv->texenv_color, 4);
        wv->ve3_bound = TRUE;
    } else {
        /* ORIGINAL 2-attr path (REPLACE / untextured).  Rebind VE *only* to undo a
         * prior wide draw's VE3 bind -- so an all-REPLACE workload (the cow) emits
         * the EXACT original command stream (no per-draw VE bind). */
        if (wv->ve3_bound) {
            virgl_cmd_bind_object(&cbuf, VIRGL_OBJECT_VERTEX_ELEMENTS, wv->info.ve_handle);
            wv->ve3_bound = FALSE;
        }
        virgl_cmd_bind_shader(&cbuf, PIPE_SHADER_VERTEX, wv->info.vs_handle);
        if (ti) {
            uint32 samp = tex_sampler(&cbuf, wv, ti);
            virgl_cmd_bind_shader(&cbuf, PIPE_SHADER_FRAGMENT, wv->info.fs_tex_handle);
            virgl_cmd_bind_sampler_states(&cbuf, PIPE_SHADER_FRAGMENT, 0, 1, &samp);
            virgl_cmd_set_sampler_views(&cbuf, PIPE_SHADER_FRAGMENT, 0, 1, &ti->view);
        } else {
            virgl_cmd_bind_shader(&cbuf, PIPE_SHADER_FRAGMENT, wv->info.fs_handle);
        }
    }

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
            virgl_emit_float(&cbuf, 2.0f * y / fbh - 1.0f);   /* ndc y (backend present blit is NOT Y-flipped) */
            virgl_emit_float(&cbuf, z);                       /* ndc z -> depth */
            virgl_emit_float(&cbuf, 1.0f);                    /* w */
        }
        if (wide) {
            /* GENERIC[0]=texcoord, GENERIC[1]=vertex colour (12 floats/vert). */
            const float *t = (const float *)(v + wv->ia_tcoord_off);
            const float *c = (const float *)(v + wv->ia_color_off);
            virgl_emit_float(&cbuf, t[0]); virgl_emit_float(&cbuf, t[1]);
            virgl_emit_float(&cbuf, 0.0f); virgl_emit_float(&cbuf, 1.0f);
            virgl_emit_float(&cbuf, c[0]); virgl_emit_float(&cbuf, c[1]);
            virgl_emit_float(&cbuf, c[2]); virgl_emit_float(&cbuf, c[3]);
        } else if (ti) {
            /* texcoord -> GENERIC[0]; FS_TEX samples .xy.  V follows the
             * geometry (the NDC Y-flip is a rigid flip; per-vertex UVs ride
             * along with it), so do NOT flip V here. */
            const float *t = (const float *)(v + wv->ia_tcoord_off);
            virgl_emit_float(&cbuf, t[0]);   /* u */
            virgl_emit_float(&cbuf, t[1]);   /* v */
            virgl_emit_float(&cbuf, 0.0f);
            virgl_emit_float(&cbuf, 1.0f);
        } else if (wv->ia_has_color) {
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

    vb.stride = wide ? 48 : 32; vb.buffer_offset = 0; vb.res_handle = wv->info.vbuf_res;
    virgl_cmd_set_vertex_buffers(&cbuf, 1, &vb);
    virgl_cmd_draw_vbo(&cbuf, 0, nverts, pipe_prim, 0, 1, 0, 0, 0, 0,
                       0, nverts - 1, 0);

    /* NEVER submit a truncated stream: virglrenderer would report "Illegal
     * command buffer" and poison the host context (display freezes for good
     * while the guest keeps running).  Dropping this chunk instead costs one
     * partial draw and a serial line -- fail-visible, ctx stays healthy. */
    if (cbuf.overflowed) {
        DW3D("draw_chunk: cmd buffer OVERFLOWED (%lu/%lu dwords, %lu verts %s) -- DROPPED\n",
             (unsigned long)cbuf.dwords, (unsigned long)cbuf.max_dwords,
             (unsigned long)nverts, wide ? "wide" : "narrow");
        return FALSE;
    }

    return g_IV3D->Submit(g_IV3D, wv->info.token, wv->info.ctx_id,
                          cbuf.buf, cbuf.dwords);
}

uint32 w3d_DrawElements(struct Warp3DIFace *Self, W3D_Context *ctx,
                        uint32 prim, uint32 type, uint32 count, void *indices)
{
    struct W3DVirgl *wv;
    uint32 idx_size, pipe_prim, done, CHUNK;
    (void)Self;

    if (!ctx || !ctx->driver) return W3D_ILLEGALINPUT;
    wv = ctx->driver;
    /* The FE tracks the live W3D state bits in ctx+0x1c (W3D_SetState only hands the
     * backend a per-state selector, not the enable/disable -- that lives here).  Read
     * it per-draw to drive the depth-test/write + blend DSA selection.  This is how
     * the cow's 2D Cosmos overlay (W3D_ZBUFFER disabled) gets depth-off so it
     * composites over the scene. */
    if ((uint32)(APTR)ctx >= 0x10000000 && (uint32)(APTR)ctx < 0x80000000) {
        uint32 fe = *(volatile uint32 *)((UBYTE *)ctx + 0x1c);
        wv->depth_test  = (fe & W3D_ZBUFFER)       != 0;
        wv->depth_write = (fe & W3D_ZBUFFERUPDATE) != 0;
        wv->blend_on    = (fe & W3D_BLENDING)      != 0;
        wv->alpha_on    = (fe & W3D_ALPHATEST)     != 0;
        wv->cull_on     = (fe & W3D_CULLFACE)      != 0;
        /* W3D_SetFrontFace never reaches the backend -- the FE records it in
         * the public ctx->FrontFaceOrder (V4 field), like texenv.  INVERTED
         * into GL terms: our pipeline rasterizes with an effective y-flip
         * (suite-proven: a screen-space-CCW quad is window-CW), so
         * W3D_CCW-front == GL front_ccw 0. */
        wv->front_ccw   = (ctx->FrontFaceOrder == W3D_CW);
        /* TexEnv is PER-TEXTURE in classic W3D: W3D_SetTexEnv dispatches to
         * backend slot 35 and the FE stores NOTHING in the public context --
         * ctx->globaltexenvmode only carries the context default (MODULATE on
         * the stock FE).  Use the bound texture's recorded mode when set
         * (w3d_suite proved SetTexEnv never reaches the globals). */
        wv->texenv_mode = ctx->globaltexenvmode ? ctx->globaltexenvmode : W3D_REPLACE;
        wv->texenv_color[0] = ctx->globaltexenvcolor[0];
        wv->texenv_color[1] = ctx->globaltexenvcolor[1];
        wv->texenv_color[2] = ctx->globaltexenvcolor[2];
        wv->texenv_color[3] = ctx->globaltexenvcolor[3];
        if (wv->cur_tex && wv->cur_tex->driver) {
            struct W3DTexInfo *cti = (struct W3DTexInfo *)wv->cur_tex->driver;
            if (cti->magic == W3DTEX_MAGIC && cti->texenv_mode) {
                wv->texenv_mode = cti->texenv_mode;
                wv->texenv_color[0] = cti->texenv_color[0];
                wv->texenv_color[1] = cti->texenv_color[1];
                wv->texenv_color[2] = cti->texenv_color[2];
                wv->texenv_color[3] = cti->texenv_color[3];
            }
        }
    }
    /* Cap each submit so INLINE_WRITE + ALL surrounding state commands fit the
     * SUBMIT_3D buffer (W3D_CMDBUF_DWORDS=16384).  Multiple of 3 so a TRIANGLES
     * chunk never splits a triangle.  Wide TexEnv path = 12 floats/vert -> 1350
     * (=450*3): 12+16200 IW plus worst-case state (fb/viewport/scissor/DSA/
     * blend-create/clear/binds/samplers/constbuf/svb/draw ~= 92 dwords) = 16304.
     * The original 1359 left only 64 dwords -- the FIRST cow frame (clear +
     * blend-create + draw in one submit) overflowed by ~10, virglrenderer
     * reported "Illegal command buffer" and poisoned the host context = the
     * June 2026 grey-window freeze.  REPLACE path = 8 floats/vert -> 1998
     * (=666*3), unchanged and historically proven. */
    CHUNK = (wv->texenv_mode != W3D_REPLACE) ? 1338U : 1998U;
    /* Transition-only census: one line per texenv MODE CHANGE tells us which
     * path (WIDE combine vs narrow REPLACE) a real workload actually runs --
     * the June grey-window revert lacked exactly this visibility. */
    if (wv->texenv_mode != wv->texenv_mode_logged) {
        DW3D("texenv mode %lu -> %lu (%s path)\n",
             (unsigned long)wv->texenv_mode_logged, (unsigned long)wv->texenv_mode,
             (wv->texenv_mode == W3D_REPLACE) ? "narrow" : "WIDE");
        wv->texenv_mode_logged = wv->texenv_mode;
    }
    DW3D("DrawElements: prim=%lu count=%lu\n", (unsigned long)prim, (unsigned long)count);
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
    DW3D("ClearDrawRegion: argb=%08lx\n", (unsigned long)color);

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
    /* 64 dwords: bind_rt_framebuffer is ~20 (fb 4 + viewport 8 + scissor 4 +
     * DSA 2 + blend 2, +11 more if the blend object is dirty) + clear 9.  The
     * original 16 truncated EXACTLY after fb+viewport+scissor -- a valid
     * stream with the clear itself silently dropped, so a clear-only frame
     * never cleared (w3d_suite check 1 caught it; real apps always draw after
     * clearing, which emits the clear in draw_elements_chunk instead). */
    uint32 cmd_words[64];
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
        virgl_cmd_init(&cbuf, cmd_words, 64);
        bind_rt_framebuffer(&cbuf, wv);
        virgl_cmd_clear(&cbuf, 5 /*COLOR0|DEPTH*/, cr, cg, cb, ca, 1.0, 0);
        if (cbuf.overflowed) {
            DW3D("Flush: clear-only cmd buffer OVERFLOWED (%lu/%lu) -- DROPPED\n",
                 (unsigned long)cbuf.dwords, (unsigned long)cbuf.max_dwords);
        } else {
            g_IV3D->Submit(g_IV3D, wv->info.token, wv->info.ctx_id,
                           cbuf.buf, cbuf.dwords);
        }
        wv->pending_clear = FALSE;
    }
    return W3D_SUCCESS;
}

void w3d_FlushFrame(struct Warp3DIFace *Self, W3D_Context *ctx)
{
    struct W3DVirgl *wv;
    (void)Self;

    w3d_Flush(Self, ctx);            /* finish any pending clear */
    if (!ctx || !ctx->driver) return;
    wv = ctx->driver;
    DW3D("FlushFrame: present\n");
    frame_present(wv, (struct BitMap *)ctx->drawregion);
}
