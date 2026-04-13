/*
 * chip_composite.c -- Hardware compositing via Virgl textured quads.
 *
 * Provides source bitmap upload (AmigaOS BitMap -> GPU texture),
 * textured quad rendering, and resource lifecycle management for
 * CompositeTags() hardware acceleration.
 *
 * Pixel format note (IMPORTANT):
 *   PPC RGBFB_A8R8G8B8 stores uint32 0xAARRGGBB -> memory bytes AA,RR,GG,BB.
 *   virgl_emit_dword (GP32) preserves the uint32 value on the LE host.
 *   On the host, uint32 0xAARRGGBB has LE bytes BB,GG,RR,AA.
 *   PIPE_FORMAT_B8G8R8A8_UNORM expects byte layout B,G,R,A -> matches. [OK]
 *
 *   So we upload RGBFB_A8R8G8B8 pixels via virgl_emit_dword and declare
 *   the texture as PIPE_FORMAT_B8G8R8A8_UNORM.
 */

#include "chip/chip_state.h"
#include "virgl/virgl_cmd.h"
#include <graphics/composite.h>
#include <graphics/displayinfo.h>
#include <graphics/modeid.h>
#include <interfaces/graphics.h>
#include <utility/tagitem.h>

/* -----------------------------------------------------------------------
 * Handle pool management.
 *
 * Compositing uses temporary Virgl objects (resources, sampler views).
 * Handle ID space layout in this driver:
 *   0-99    reserved / virglrenderer built-ins
 *   100-119 permanent pipeline state objects from chip_virgl_2d.c
 *           (blend, DSA, rasterizer, surface, VS, FS_COLOR, FS_TEX,
 *           VE, samplers -- 12 slots used, 20 reserved for growth)
 *   200+    compositing temporaries, incremented per allocation
 * The 200 base leaves an 80-slot buffer between the permanent and
 * temporary ranges so new pipeline objects can be added without
 * renumbering compositing.
 * ----------------------------------------------------------------------- */
#define COMP_HANDLE_BASE  200

static uint32 comp_alloc_handle(struct ChipGPUState *gs)
{
    if (gs->comp_next_handle == 0)
        gs->comp_next_handle = COMP_HANDLE_BASE;
    return gs->comp_next_handle++;
}

/* -----------------------------------------------------------------------
 * Porter-Duff blend state creation.
 *
 * Each of the 15 AmigaOS compositing operators maps to a set of GPU
 * blend factors.  We create one Virgl blend object per operator during
 * init and store the handles in gs->comp_blends[].
 *
 * Operators 13 (Maximum) and 14 (Minimum) require GL_MAX/GL_MIN blend
 * equations which aren't exposed through our blend state encoding,
 * so they remain 0 (= fall back to software).
 * ----------------------------------------------------------------------- */

/* Helper: build the per-RT blend word from separate src/dst factors
 * for RGB and Alpha channels.  Always uses FUNC_ADD. */
static uint32 comp_blend_rt(uint32 src_rgb, uint32 dst_rgb,
                              uint32 src_a,   uint32 dst_a)
{
    return VIRGL_BLEND_RT_BLEND_ENABLE(1) |
           VIRGL_BLEND_RT_RGB_FUNC(PIPE_BLEND_ADD) |
           VIRGL_BLEND_RT_RGB_SRC_FACTOR(src_rgb) |
           VIRGL_BLEND_RT_RGB_DST_FACTOR(dst_rgb) |
           VIRGL_BLEND_RT_ALPHA_FUNC(PIPE_BLEND_ADD) |
           VIRGL_BLEND_RT_ALPHA_SRC_FACTOR(src_a) |
           VIRGL_BLEND_RT_ALPHA_DST_FACTOR(dst_a) |
           VIRGL_BLEND_RT_COLORMASK(0xF);
}

/* Porter-Duff factor table indexed by enPDOperator (graphics/composite.h).
 * Each row: {src_rgb_factor, dst_rgb_factor, src_alpha_factor, dst_alpha_factor}.
 * Values are PIPE_BLENDFACTOR_* constants from the Virgl/Gallium layer.
 * Table covers COMPOSITE_Clear .. COMPOSITE_Plus (operators 0..12).
 * COMPOSITE_Maximum (13) and COMPOSITE_Minimum (14) fall through to the
 * software path -- they require min/max blend ops not exposed by this
 * subset of Virgl commands. */
static const uint8 pd_factors[13][4] = {
    /* 0  Clear       */ { PIPE_BLENDFACTOR_ZERO,          PIPE_BLENDFACTOR_ZERO,
                           PIPE_BLENDFACTOR_ZERO,          PIPE_BLENDFACTOR_ZERO },
    /* 1  Src         */ { PIPE_BLENDFACTOR_ONE,           PIPE_BLENDFACTOR_ZERO,
                           PIPE_BLENDFACTOR_ONE,           PIPE_BLENDFACTOR_ZERO },
    /* 2  Dest        */ { PIPE_BLENDFACTOR_ZERO,          PIPE_BLENDFACTOR_ONE,
                           PIPE_BLENDFACTOR_ZERO,          PIPE_BLENDFACTOR_ONE },
    /* 3  SrcOverDest */ { PIPE_BLENDFACTOR_SRC_ALPHA,     PIPE_BLENDFACTOR_INV_SRC_ALPHA,
                           PIPE_BLENDFACTOR_ONE,           PIPE_BLENDFACTOR_INV_SRC_ALPHA },
    /* 4  DestOverSrc */ { PIPE_BLENDFACTOR_INV_DST_ALPHA, PIPE_BLENDFACTOR_ONE,
                           PIPE_BLENDFACTOR_INV_DST_ALPHA, PIPE_BLENDFACTOR_ONE },
    /* 5  SrcInDest   */ { PIPE_BLENDFACTOR_DST_ALPHA,     PIPE_BLENDFACTOR_ZERO,
                           PIPE_BLENDFACTOR_DST_ALPHA,     PIPE_BLENDFACTOR_ZERO },
    /* 6  DestInSrc   */ { PIPE_BLENDFACTOR_ZERO,          PIPE_BLENDFACTOR_SRC_ALPHA,
                           PIPE_BLENDFACTOR_ZERO,          PIPE_BLENDFACTOR_SRC_ALPHA },
    /* 7  SrcOutDest  */ { PIPE_BLENDFACTOR_INV_DST_ALPHA, PIPE_BLENDFACTOR_ZERO,
                           PIPE_BLENDFACTOR_INV_DST_ALPHA, PIPE_BLENDFACTOR_ZERO },
    /* 8  DestOutSrc  */ { PIPE_BLENDFACTOR_ZERO,          PIPE_BLENDFACTOR_INV_SRC_ALPHA,
                           PIPE_BLENDFACTOR_ZERO,          PIPE_BLENDFACTOR_INV_SRC_ALPHA },
    /* 9  SrcAtopDest */ { PIPE_BLENDFACTOR_DST_ALPHA,     PIPE_BLENDFACTOR_INV_SRC_ALPHA,
                           PIPE_BLENDFACTOR_ZERO,          PIPE_BLENDFACTOR_ONE },
    /* 10 DestAtopSrc */ { PIPE_BLENDFACTOR_INV_DST_ALPHA, PIPE_BLENDFACTOR_SRC_ALPHA,
                           PIPE_BLENDFACTOR_ONE,           PIPE_BLENDFACTOR_ZERO },
    /* 11 SrcXorDest  */ { PIPE_BLENDFACTOR_INV_DST_ALPHA, PIPE_BLENDFACTOR_INV_SRC_ALPHA,
                           PIPE_BLENDFACTOR_INV_DST_ALPHA, PIPE_BLENDFACTOR_INV_SRC_ALPHA },
    /* 12 Plus        */ { PIPE_BLENDFACTOR_ONE,           PIPE_BLENDFACTOR_ONE,
                           PIPE_BLENDFACTOR_ONE,           PIPE_BLENDFACTOR_ONE },
};

/* -----------------------------------------------------------------------
 * chip_comp_init_blends -- Create blend objects for all Porter-Duff operators.
 *
 * Called once during virgl_init_2d.  Creates 13 blend objects (ops 0-12)
 * in a single Virgl submit.  Operators 13-14 are left as 0 (software).
 *
 * Returns TRUE if all blend objects were created successfully.
 * ----------------------------------------------------------------------- */
BOOL chip_comp_init_blends(struct ChipGPUState *gs)
{
    if (!gs->virgl_2d_ready)
        return FALSE;

    uint32 ctx_id = gs->virgl_2d_ctx;
    BOOL ok = TRUE;

    for (uint32 op = 0; op <= 12 && ok; op++) {
        uint32 handle = comp_alloc_handle(gs);
        uint32 rt = comp_blend_rt(pd_factors[op][0], pd_factors[op][1],
                                    pd_factors[op][2], pd_factors[op][3]);

        uint32 cmd_words[16];
        struct VirglCmdBuf cbuf;
        virgl_cmd_init(&cbuf, cmd_words, 16);
        virgl_cmd_create_blend(&cbuf, handle, 0, rt);

        ok = virgl_submit(gs, ctx_id, &cbuf);
        if (ok) {
            gs->comp_blends[op] = handle;
        } else {
            DCHIP("comp_init_blends: FAILED at op=%lu handle=%lu",
                  (unsigned long)op, (unsigned long)handle);
        }
    }

    /* Ops 13 (Maximum) and 14 (Minimum) = 0 -> software fallback */
    gs->comp_blends[13] = 0;
    gs->comp_blends[14] = 0;

    if (ok)
        DCHIP("comp_init_blends: 13 blend states created OK");
    return ok;
}

/* -----------------------------------------------------------------------
 * chip_comp_upload_pixels_32bpp -- Upload 32bpp pixel strip via INLINE_WRITE.
 *
 * Uploads one horizontal strip of pixels from a 32bpp source bitmap into
 * a previously created 3D resource.  Each pixel is emitted as a uint32
 * through virgl_emit_dword (GP32-swapped for correct LE host format).
 *
 * The strip is limited by the Virgl command buffer size.  The caller
 * must split large bitmaps into multiple strips.
 *
 * Parameters:
 *   gs       -- GPU state (for submit)
 *   res      -- target 3D resource handle
 *   src      -- pointer to first pixel of the strip (uint32 array)
 *   src_bpr  -- source bytes per row (may differ from width*4 due to padding)
 *   x, y     -- destination offset within the resource
 *   w, h     -- strip dimensions in pixels
 * ----------------------------------------------------------------------- */
static BOOL chip_comp_upload_pixels_32bpp(struct ChipGPUState *gs,
                                           uint32 res, const uint32 *src,
                                           uint32 src_bpr,
                                           uint32 x, uint32 y,
                                           uint32 w, uint32 h)
{
    uint32 ctx_id = gs->virgl_2d_ctx;
    uint32 stride = w * 4;

    /* Command buffer sizing for INLINE_WRITE texture uploads.
     *
     * The Virgl command buffer is capped at approximately 16K dwords per
     * submission (virglrenderer's internal receive buffer is 64KB and each
     * dword is 4 bytes; some headroom is needed for framing).  A single
     * INLINE_WRITE packet consumes 12 header dwords (cmd+length word,
     * resource handle, level, stride/layer, box x/y/z/w/h/d, data offset)
     * followed by the pixel payload at 1 dword per B8G8R8A8 pixel.
     *
     * So: max payload ~ (16000 - 12) = 15988 dwords.  We use 15000 as a
     * conservative cap -- leaves margin for virglrenderer version drift and
     * for any growth in the header format.  Larger textures are uploaded
     * as multiple row-aligned strips. */
    uint32 max_pixels_per_submit = 15000;
    uint32 max_rows = max_pixels_per_submit / w;
    if (max_rows == 0) max_rows = 1;
    if (max_rows > h) max_rows = h;

    /* Allocate command buffer on stack -- sized for one strip.
     * Each submit: 12 header + w*rows data = at most 12 + 15000 words. */
    uint32 cbuf_size = 12 + (w * max_rows) + 16;  /* +16 margin */
    if (cbuf_size > 16000) cbuf_size = 16000;

    /* Use heap allocation for the command buffer (too large for stack) */
    uint32 *cmd_words = (uint32 *)gs->IExec->AllocVecTags(
        cbuf_size * sizeof(uint32),
        AVT_Type, MEMF_PRIVATE,
        TAG_DONE);
    if (!cmd_words) {
        DCHIP("comp_upload: AllocVec failed for cmd_words (%lu)", cbuf_size);
        return FALSE;
    }

    BOOL ok = TRUE;
    uint32 rows_done = 0;

    while (rows_done < h && ok) {
        uint32 rows_this = h - rows_done;
        if (rows_this > max_rows) rows_this = max_rows;

        struct VirglCmdBuf cbuf;
        virgl_cmd_init(&cbuf, cmd_words, cbuf_size);

        /* INLINE_WRITE header */
        uint32 data_words = w * rows_this;
        uint32 payload_len = 11 + data_words;

        virgl_emit_dword(&cbuf, VIRGL_CMD_HDR(VIRGL_CCMD_RESOURCE_INLINE_WRITE,
                                                0, payload_len));
        virgl_emit_dword(&cbuf, res);
        virgl_emit_dword(&cbuf, 0);          /* level */
        virgl_emit_dword(&cbuf, 0);          /* usage */
        virgl_emit_dword(&cbuf, stride);     /* stride */
        virgl_emit_dword(&cbuf, 0);          /* layer_stride */
        virgl_emit_dword(&cbuf, x);          /* box.x */
        virgl_emit_dword(&cbuf, y + rows_done); /* box.y */
        virgl_emit_dword(&cbuf, 0);          /* box.z */
        virgl_emit_dword(&cbuf, w);          /* box.w */
        virgl_emit_dword(&cbuf, rows_this);  /* box.h */
        virgl_emit_dword(&cbuf, 1);          /* box.d */

        /* Pixel data -- each uint32 pixel is GP32-swapped by virgl_emit_dword.
         * Source rows may have padding (src_bpr > w*4), so step row-by-row. */
        for (uint32 row = 0; row < rows_this; row++) {
            const uint32 *row_src = (const uint32 *)
                ((const uint8 *)src + (rows_done + row) * src_bpr);
            for (uint32 col = 0; col < w; col++) {
                virgl_emit_dword(&cbuf, row_src[col]);
            }
        }

        ok = virgl_submit(gs, ctx_id, &cbuf);
        if (!ok) {
            DCHIP("comp_upload: INLINE_WRITE submit failed at row %lu",
                  (unsigned long)(y + rows_done));
        }
        rows_done += rows_this;
    }

    gs->IExec->FreeVec(cmd_words);
    return ok;
}

/* -----------------------------------------------------------------------
 * chip_comp_upload_bitmap -- Upload a bitmap region as a GPU texture.
 *
 * Creates a temporary 3D resource, uploads the specified rectangle of
 * pixel data from a source bitmap, and creates a sampler view for
 * shader sampling.
 *
 * Currently supports RGBFB_A8R8G8B8 (32bpp with alpha) only.
 * Other formats cause an early return with FALSE.
 *
 * The caller must call chip_comp_free_texture() after rendering to
 * release the temporary resource.
 *
 * Parameters:
 *   gs        -- GPU state
 *   src_data  -- pointer to first pixel of the source region (uint32*)
 *   src_bpr   -- source bytes per row
 *   src_w     -- width of the region to upload (pixels)
 *   src_h     -- height of the region to upload (pixels)
 *   has_alpha -- TRUE if source has per-pixel alpha (A8R8G8B8)
 *
 * On success, gs->comp_tex_res and gs->comp_tex_view are set.
 * ----------------------------------------------------------------------- */
BOOL chip_comp_upload_bitmap(struct ChipGPUState *gs,
                              const void *src_data, uint32 src_bpr,
                              uint32 src_w, uint32 src_h,
                              BOOL has_alpha)
{
    if (!gs->virgl_2d_ready || !gs->virgl_shaders_ok)
        return FALSE;
    if (src_w == 0 || src_h == 0 || !src_data)
        return FALSE;

    /* Sanity limit -- very large textures should fall back to software.
     * 1024x1024 at 4 bytes/pixel = 4MB, reasonable for QEMU. */
    if (src_w > 1024 || src_h > 1024) {
        DCHIP("comp_upload: bitmap too large (%lux%lu), max 1024x1024",
              (unsigned long)src_w, (unsigned long)src_h);
        return FALSE;
    }

    /* Free any previous temp texture (shouldn't happen, but be safe) */
    if (gs->comp_tex_res)
        chip_comp_free_texture(gs);

    uint32 ctx_id = gs->virgl_2d_ctx;

    /* Choose GPU format based on alpha.
     * RGBFB_A8R8G8B8 -> PIPE_FORMAT_B8G8R8A8_UNORM (see header comment).
     * For no-alpha sources, use B8G8R8X8_UNORM (alpha reads as 1.0). */
    uint32 pipe_fmt = has_alpha ? PIPE_FORMAT_B8G8R8A8_UNORM
                                : PIPE_FORMAT_B8G8R8X8_UNORM;

    /* Allocate a 3D resource for the texture */
    uint32 res_id = chip_alloc_resource_id(gs);

    if (!chip_ResourceCreate3D(gs, res_id,
            PIPE_TEXTURE_2D,
            pipe_fmt,
            PIPE_BIND_SAMPLER_VIEW,
            src_w, src_h, 1,   /* width, height, depth */
            1, 0, 0, 0))       /* array_size, last_level, samples, flags */
    {
        DCHIP("comp_upload: RESOURCE_CREATE_3D failed (%lux%lu fmt=%lu)",
              (unsigned long)src_w, (unsigned long)src_h, (unsigned long)pipe_fmt);
        return FALSE;
    }

    /* Attach resource to the Virgl context */
    chip_CTXAttachResource(gs, ctx_id, res_id);

    DCHIP("comp_upload: res=%lu %lux%lu fmt=%lu created",
          (unsigned long)res_id,
          (unsigned long)src_w, (unsigned long)src_h,
          (unsigned long)pipe_fmt);

    /* Upload pixel data via INLINE_WRITE (strip by strip) */
    if (!chip_comp_upload_pixels_32bpp(gs, res_id,
            (const uint32 *)src_data, src_bpr, 0, 0, src_w, src_h))
    {
        DCHIP("comp_upload: pixel upload failed, destroying res=%lu",
              (unsigned long)res_id);
        chip_CTXDetachResource(gs, ctx_id, res_id);
        chip_ResourceUnref(gs, res_id);
        return FALSE;
    }

    /* Create sampler view for shader sampling */
    uint32 view_handle = comp_alloc_handle(gs);

    {
        uint32 cmd_words[32];
        struct VirglCmdBuf cbuf;
        virgl_cmd_init(&cbuf, cmd_words, 32);

        virgl_cmd_create_sampler_view(&cbuf, view_handle,
            res_id, pipe_fmt,
            0, 0,  /* first_element, last_element (mip levels) */
            PIPE_SWIZZLE_RED, PIPE_SWIZZLE_GREEN,
            PIPE_SWIZZLE_BLUE, PIPE_SWIZZLE_ALPHA);

        if (!virgl_submit(gs, ctx_id, &cbuf)) {
            DCHIP("comp_upload: sampler view creation failed");
            chip_CTXDetachResource(gs, ctx_id, res_id);
            chip_ResourceUnref(gs, res_id);
            return FALSE;
        }
    }

    /* Record temp texture state */
    gs->comp_tex_res  = res_id;
    gs->comp_tex_view = view_handle;
    gs->comp_tex_w    = src_w;
    gs->comp_tex_h    = src_h;

    DCHIP("comp_upload: OK res=%lu view=%lu %lux%lu",
          (unsigned long)res_id, (unsigned long)view_handle,
          (unsigned long)src_w, (unsigned long)src_h);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * chip_comp_free_texture -- Release temporary texture resources.
 *
 * Destroys the sampler view and 3D resource created by
 * chip_comp_upload_bitmap().  Safe to call if no texture is allocated.
 * ----------------------------------------------------------------------- */
void chip_comp_free_texture(struct ChipGPUState *gs)
{
    if (!gs->comp_tex_res)
        return;

    uint32 ctx_id = gs->virgl_2d_ctx;

    /* Destroy sampler view, then detach + destroy resource */
    {
        uint32 cmd_words[16];
        struct VirglCmdBuf cbuf;
        virgl_cmd_init(&cbuf, cmd_words, 16);

        if (gs->comp_tex_view) {
            virgl_cmd_destroy_object(&cbuf, VIRGL_OBJECT_SAMPLER_VIEW,
                                      gs->comp_tex_view);
        }

        virgl_submit(gs, ctx_id, &cbuf);
    }

    chip_CTXDetachResource(gs, ctx_id, gs->comp_tex_res);
    chip_ResourceUnref(gs, gs->comp_tex_res);

    DCHIP("comp_free: destroyed res=%lu view=%lu",
          (unsigned long)gs->comp_tex_res,
          (unsigned long)gs->comp_tex_view);

    gs->comp_tex_res  = 0;
    gs->comp_tex_view = 0;
    gs->comp_tex_w    = 0;
    gs->comp_tex_h    = 0;
}

/* -----------------------------------------------------------------------
 * chip_comp_draw_textured_quad -- Draw a textured quad via DRAW_VBO.
 *
 * Binds the texture FS, sampler, and sampler view, then renders a
 * textured quad at the given pixel coordinates on the framebuffer.
 *
 * The source texture is sampled over the region (s0,t0)-(s1,t1) in
 * normalised UV space [0..1].  The quad is placed at pixel coords
 * (dst_x, dst_y, dst_w, dst_h) on the framebuffer.
 *
 * Assumes the temp texture was uploaded via chip_comp_upload_bitmap().
 *
 * Parameters:
 *   gs           -- GPU state
 *   dst_x/y/w/h  -- destination rectangle in pixels
 *   s0,t0,s1,t1  -- UV coordinates for texture sampling
 *   use_linear   -- TRUE for bilinear filtering, FALSE for nearest
 * ----------------------------------------------------------------------- */
BOOL chip_comp_draw_textured_quad(struct ChipGPUState *gs,
                                    uint32 dst_x, uint32 dst_y,
                                    uint32 dst_w, uint32 dst_h,
                                    float s0, float t0, float s1, float t1,
                                    BOOL use_linear)
{
    if (!gs->virgl_2d_ready || !gs->virgl_shaders_ok)
        return FALSE;
    if (!gs->comp_tex_view || !gs->virgl_2d_vbuf_res)
        return FALSE;

    uint32 ctx_id = gs->virgl_2d_ctx;
    uint32 fb_w = gs->fb_width;
    uint32 fb_h = gs->fb_height;

    /* Convert pixel coords to NDC.  Viewport: [-1,+1] -> [0,fb_w/h]. */
    float x0 = 2.0f * (float)dst_x / (float)fb_w - 1.0f;
    float y0 = 2.0f * (float)dst_y / (float)fb_h - 1.0f;
    float x1 = 2.0f * (float)(dst_x + dst_w) / (float)fb_w - 1.0f;
    float y1 = 2.0f * (float)(dst_y + dst_h) / (float)fb_h - 1.0f;

    /* 6 vertices x 8 floats (pos[4] + texcoord[4]).
     * Texcoord uses .xy for (s,t), .zw = (0,1) for padding. */
    float verts[48] = {
        /* Triangle 1: top-left, top-right, bottom-right */
        x0, y0, 0.0f, 1.0f,   s0, t0, 0.0f, 1.0f,
        x1, y0, 0.0f, 1.0f,   s1, t0, 0.0f, 1.0f,
        x1, y1, 0.0f, 1.0f,   s1, t1, 0.0f, 1.0f,
        /* Triangle 2: top-left, bottom-right, bottom-left */
        x0, y0, 0.0f, 1.0f,   s0, t0, 0.0f, 1.0f,
        x1, y1, 0.0f, 1.0f,   s1, t1, 0.0f, 1.0f,
        x0, y1, 0.0f, 1.0f,   s0, t1, 0.0f, 1.0f,
    };

    /* Command buffer:
     *   - bind texture FS (2 words)
     *   - bind sampler state (4 words)
     *   - set sampler view (4 words)
     *   - INLINE_WRITE vertices (12 + 48 = 60 words)
     *   - SET_VERTEX_BUFFERS (4 words)
     *   - DRAW_VBO (13 words)
     * Total ~ 87 words.  Use 128 for margin. */
    uint32 cmd_words[128];
    struct VirglCmdBuf cbuf;
    virgl_cmd_init(&cbuf, cmd_words, 128);

    /* Bind texture fragment shader (replacing color FS) */
    virgl_cmd_bind_shader(&cbuf, PIPE_SHADER_FRAGMENT, gs->virgl_2d_fs_tex);

    /* Bind sampler state (nearest or linear) to fragment stage slot 0 */
    {
        uint32 sampler = use_linear ? gs->virgl_2d_sampler_linear
                                    : gs->virgl_2d_sampler;
        virgl_cmd_bind_sampler_states(&cbuf, PIPE_SHADER_FRAGMENT, 0, 1,
                                       &sampler);
    }

    /* Bind sampler view (temp texture) to fragment stage slot 0 */
    virgl_cmd_set_sampler_views(&cbuf, PIPE_SHADER_FRAGMENT, 0, 1,
                                 &gs->comp_tex_view);

    /* Upload vertex data */
    virgl_upload_vertex_floats(&cbuf, gs->virgl_2d_vbuf_res, verts, 48);

    /* Bind vertex buffer: stride=32, offset=0 */
    {
        struct VirglVertexBuffer vb;
        vb.stride = 32;
        vb.buffer_offset = 0;
        vb.res_handle = gs->virgl_2d_vbuf_res;
        virgl_cmd_set_vertex_buffers(&cbuf, 1, &vb);
    }

    /* Draw 6 vertices as 2 triangles */
    virgl_cmd_draw_vbo(&cbuf,
        0, 6, PIPE_PRIM_TRIANGLES,
        0, 1,       /* indexed=0, instance_count=1 */
        0, 0,       /* index_bias, start_instance */
        0, 0,       /* primitive_restart, restart_index */
        0, 5,       /* min_index, max_index */
        0);         /* cso_handle */

    BOOL ok = virgl_submit(gs, ctx_id, &cbuf);

    if (ok) {
        /* Flush the drawn region to the display */
        chip_ResourceFlush(gs, gs->resource_id, dst_x, dst_y, dst_w, dst_h);
    } else {
        DCHIP("comp_draw: SUBMIT FAILED");
    }

    /* Restore color FS for subsequent non-textured draws (FillRect etc.) */
    {
        uint32 restore_words[8];
        struct VirglCmdBuf rcbuf;
        virgl_cmd_init(&rcbuf, restore_words, 8);
        virgl_cmd_bind_shader(&rcbuf, PIPE_SHADER_FRAGMENT, gs->virgl_2d_fs);
        virgl_submit(gs, ctx_id, &rcbuf);
    }

    return ok;
}

/* -----------------------------------------------------------------------
 * chip_virgl_composite -- Main compositing entry point.
 *
 * Performs a single Porter-Duff compositing operation:
 *   1. Validates operator and flags
 *   2. Uploads source bitmap as GPU texture (or uses solid color)
 *   3. Binds the blend state for the requested operator
 *   4. Draws a textured (or colored) quad at the destination
 *   5. Flushes the result to the display
 *   6. Cleans up temporary resources
 *
 * Parameters:
 *   gs            -- GPU state
 *   op            -- Porter-Duff operator (COMPOSITE_* enum, 0..14)
 *   src_bm        -- source bitmap, or COMPSRC_SOLIDCOLOR for solid fill
 *   src_data      -- pointer to first pixel of source region (NULL for solid)
 *   src_bpr       -- source bytes per row
 *   src_format    -- source pixel format (RGBFB_*)
 *   src_x/y/w/h   -- source rectangle
 *   dst_x/y/w/h   -- destination rectangle on framebuffer
 *   flags         -- COMPFLAG_* flags
 *   color0        -- ARGB color for COMPSRC_SOLIDCOLOR
 *
 * Returns COMPERR_* error code.
 * ----------------------------------------------------------------------- */
uint32 chip_virgl_composite(struct ChipGPUState *gs,
                              uint32 op,
                              struct BitMap *src_bm,
                              const void *src_data,
                              uint32 src_bpr,
                              uint32 src_format,
                              int32 src_x, int32 src_y,
                              int32 src_w, int32 src_h,
                              int32 dst_x, int32 dst_y,
                              int32 dst_w, int32 dst_h,
                              uint32 flags,
                              uint32 color0)
{
    /* Limit debug to first N calls to avoid log flooding */
    static uint32 call_count = 0;
    BOOL do_log = (call_count < 20);
    call_count++;

    /* --- Validation --- */

    if (!gs->virgl_2d_ready || !gs->virgl_shaders_ok)
        return COMPERR_Generic;

    if (op >= 15) {
        if (do_log) DCHIP("composite: unknown op=%lu", (unsigned long)op);
        return COMPERR_UnknownOperator;
    }

    if (flags & COMPFLAG_ForceSoftware)
        return COMPERR_SoftwareFallback;

    /* Unsupported features -> software fallback */
    if (flags & COMPFLAG_Color1Modulate)
        return COMPERR_SoftwareFallback;

    /* No blend state for Maximum/Minimum -> software */
    if (gs->comp_blends[op] == 0)
        return COMPERR_SoftwareFallback;

    /* Validate dimensions */
    if (dst_w <= 0 || dst_h <= 0)
        return COMPERR_Value;

    /* No scaling in v1 -- src and dst must be same size */
    if (src_bm != COMPSRC_SOLIDCOLOR) {
        if (src_w != dst_w || src_h != dst_h)
            return COMPERR_SoftwareFallback;
    }

    uint32 ctx_id = gs->virgl_2d_ctx;
    BOOL is_solid = (src_bm == COMPSRC_SOLIDCOLOR);
    BOOL ok = TRUE;

    if (do_log) {
        DCHIP("composite: op=%lu %s dst=(%ld,%ld,%ld,%ld) flags=0x%lx",
              (unsigned long)op,
              is_solid ? "SOLID" : "BITMAP",
              (long)dst_x, (long)dst_y, (long)dst_w, (long)dst_h,
              (unsigned long)flags);
    }

    /* --- Bind blend state for this operator --- */
    {
        uint32 cmd_words[8];
        struct VirglCmdBuf cbuf;
        virgl_cmd_init(&cbuf, cmd_words, 8);
        virgl_cmd_bind_object(&cbuf, VIRGL_OBJECT_BLEND, gs->comp_blends[op]);
        ok = virgl_submit(gs, ctx_id, &cbuf);
        if (!ok) {
            DCHIP("composite: bind blend FAILED op=%lu", (unsigned long)op);
            return COMPERR_Generic;
        }
    }

    /* --- Source handling --- */

    if (is_solid) {
        /* Solid color: draw colored quad using color FS.
         * color0 is ARGB uint32: extract float components.
         * PPC BE uint32 0xAARRGGBB -> bytes AA,RR,GG,BB. */
        float a = (float)((color0 >> 24) & 0xFF) / 255.0f;
        float r = (float)((color0 >> 16) & 0xFF) / 255.0f;
        float g = (float)((color0 >>  8) & 0xFF) / 255.0f;
        float b = (float)((color0      ) & 0xFF) / 255.0f;

        ok = chip_virgl_draw_colored_quad(gs,
                (uint32)dst_x, (uint32)dst_y,
                (uint32)dst_w, (uint32)dst_h,
                r, g, b, a);
    } else {
        /* Bitmap source: upload as texture, draw textured quad */

        /* Only support A8R8G8B8 (32bpp with alpha) for now */
        BOOL has_alpha = (src_format == RGBFB_A8R8G8B8);
        if (src_format != RGBFB_A8R8G8B8 && src_format != RGBFB_R8G8B8A8 &&
            src_format != RGBFB_B8G8R8A8 && src_format != RGBFB_A8B8G8R8) {
            /* Non-32bpp or non-alpha format -- check if it's at least 32bpp */
            uint32 bpp = chip_format_bpp(src_format);
            if (bpp != 4) {
                return COMPERR_SoftwareFallback;
            }
            /* 32bpp without alpha (e.g. R8G8B8A8 variants) */
            has_alpha = FALSE;
        }

        /* Calculate pointer to source sub-region */
        const uint8 *src_base = (const uint8 *)src_data;
        const void *src_region = src_base + src_y * src_bpr + src_x * 4;

        if (!chip_comp_upload_bitmap(gs, src_region, src_bpr,
                                       (uint32)src_w, (uint32)src_h, has_alpha))
        {
            DCHIP("composite: upload failed (%ldx%ld)",
                  (long)src_w, (long)src_h);
            return COMPERR_OutOfMemory;
        }

        /* Use bilinear filtering if requested */
        BOOL linear = (flags & COMPFLAG_SrcFilter) ? TRUE : FALSE;

        ok = chip_comp_draw_textured_quad(gs,
                (uint32)dst_x, (uint32)dst_y,
                (uint32)dst_w, (uint32)dst_h,
                0.0f, 0.0f, 1.0f, 1.0f,  /* full UV range (no scaling) */
                linear);

        chip_comp_free_texture(gs);
    }

    /* --- Restore default blend state (opaque) --- */
    {
        uint32 cmd_words[8];
        struct VirglCmdBuf cbuf;
        virgl_cmd_init(&cbuf, cmd_words, 8);
        virgl_cmd_bind_object(&cbuf, VIRGL_OBJECT_BLEND, gs->virgl_2d_blend);
        virgl_submit(gs, ctx_id, &cbuf);
    }

    if (!ok) {
        DCHIP("composite: draw FAILED");
        return COMPERR_Generic;
    }

    return COMPERR_Success;
}

/* -----------------------------------------------------------------------
 * chip_comp_test_textured_quad -- Draw a test pattern to verify texture pipeline.
 *
 * Creates a small 4x4 checkerboard texture (red/transparent) and draws
 * it at a fixed position.  Used for development/debugging only.
 *
 * Enable by setting gs->virgl_test_quad = 2 and calling this from
 * the flush task loop.
 * ----------------------------------------------------------------------- */
BOOL chip_comp_test_textured_quad(struct ChipGPUState *gs)
{
    if (!gs->virgl_2d_ready || !gs->virgl_shaders_ok)
        return FALSE;

    /* 4x4 checkerboard: red opaque / green opaque alternating.
     * RGBFB_A8R8G8B8: uint32 0xAARRGGBB */
    static const uint32 test_pixels[16] = {
        0xFFFF0000, 0xFF00FF00, 0xFFFF0000, 0xFF00FF00,
        0xFF00FF00, 0xFFFF0000, 0xFF00FF00, 0xFFFF0000,
        0xFFFF0000, 0xFF00FF00, 0xFFFF0000, 0xFF00FF00,
        0xFF00FF00, 0xFFFF0000, 0xFF00FF00, 0xFFFF0000,
    };

    /* Upload checkerboard as texture */
    if (!chip_comp_upload_bitmap(gs, test_pixels, 4 * 4, 4, 4, TRUE)) {
        DCHIP("comp_test: upload failed");
        return FALSE;
    }

    /* Draw at (100, 80) scaled to 128x128, nearest filter */
    BOOL ok = chip_comp_draw_textured_quad(gs,
        100, 80, 128, 128,
        0.0f, 0.0f, 1.0f, 1.0f,   /* full UV range */
        FALSE);                     /* nearest filter = blocky pixels */

    /* Clean up temp texture */
    chip_comp_free_texture(gs);

    if (ok) {
        static uint32 test_count = 0;
        if (test_count < 3) {
            test_count++;
            DCHIP("comp_test: textured quad drawn OK");
        }
    }

    return ok;
}

/* =======================================================================
 * CompositeTags Hook -- intercepts graphics.library CompositeTags calls.
 *
 * We hook IGraphics->CompositeTagList at vtable offset 804 (entry 186
 * after InterfaceData).  The hook checks if the destination bitmap
 * lives in our board memory; if so, we handle it via Virgl.  Otherwise
 * we fall through to the original software implementation.
 *
 * Unsupported features (vertex arrays, alpha masks, scaling) are also
 * forwarded to software.
 * ======================================================================= */
#define IGFX_COMPOSITETAGLIST_OFFSET  804

/* Global pointer to the original CompositeTagList method */
static APTR g_orig_CompositeTagList = NULL;

/* HW vs SW composite call counters for diagnostics */
static uint32 g_comp_hw_count = 0;
static uint32 g_comp_sw_count = 0;
static uint32 g_comp_total    = 0;

static uint32 hook_CompositeTagList(struct Interface *Self,
                                      uint32 Operator,
                                      struct BitMap *Source,
                                      struct BitMap *Destination,
                                      struct TagItem *tags)
{
    typedef uint32 (*OrigFunc)(struct Interface *, uint32,
                                struct BitMap *, struct BitMap *,
                                struct TagItem *);

    /* Bail to software if driver not ready or no destination.  Logged
     * once per session per reason so a virgl-off run doesn't drown the
     * log with one line per composite call. */
    struct ChipGPUState *gs = g_chip_state;
    if (!gs || !gs->virgl_2d_ready || !Destination) {
        static volatile UBYTE warn_no_gs        = 0;
        static volatile UBYTE warn_no_virgl     = 0;
        static volatile UBYTE warn_no_dest      = 0;
        if (!gs && !warn_no_gs) {
            warn_no_gs = 1;
            DCHIP("CompositeTagList hook: g_chip_state NULL -- "
                  "every call falling back to SW (logged once)");
        } else if (gs && !gs->virgl_2d_ready && !warn_no_virgl) {
            warn_no_virgl = 1;
            DCHIP("CompositeTagList hook: virgl_2d_ready=FALSE -- "
                  "every call falling back to SW (logged once; "
                  "Virgl negotiation off => no HW composite)");
        } else if (!Destination && !warn_no_dest) {
            warn_no_dest = 1;
            DCHIP("CompositeTagList hook: Destination NULL -- "
                  "first occurrence; SW fallback (logged once)");
        }
        g_comp_sw_count++;
        g_comp_total++;
        OrigFunc orig = (OrigFunc)g_orig_CompositeTagList;
        return orig(Self, Operator, Source, Destination, tags);
    }

    /* Check if destination bitmap lives in our board memory */
    APTR dst_planes0 = Destination->Planes[0];
    BOOL is_ours = (dst_planes0 >= gs->board_mem &&
                    dst_planes0 < (APTR)((uint8 *)gs->board_mem + gs->board_mem_size));
    if (!is_ours) {
        static volatile UBYTE warn_not_ours = 0;
        if (!warn_not_ours) {
            warn_not_ours = 1;
            DCHIP("CompositeTagList hook: dst Planes[0]=%p outside board_mem "
                  "[%p..%p] -- SW fallback (logged once)",
                  dst_planes0, gs->board_mem,
                  (APTR)((UBYTE *)gs->board_mem + gs->board_mem_size));
        }
        g_comp_sw_count++;
        g_comp_total++;
        OrigFunc orig = (OrigFunc)g_orig_CompositeTagList;
        return orig(Self, Operator, Source, Destination, tags);
    }

    /* Parse COMPTAG_* tags */
    int32 src_x = 0, src_y = 0, src_w = 0, src_h = 0;
    int32 dst_x = 0, dst_y = 0, dst_w = 0, dst_h = 0;
    uint32 flags = 0;
    uint32 color0 = 0xFF000000;
    BOOL has_vertex_array = FALSE;
    BOOL has_alpha_mask = FALSE;

    if (tags) {
        struct TagItem *tag;
        struct TagItem *tstate = tags;
        while ((tag = tstate) && tag->ti_Tag != TAG_DONE) {
            switch (tag->ti_Tag) {
            case TAG_MORE:    tstate = (struct TagItem *)tag->ti_Data; continue;
            case TAG_SKIP:    tstate += tag->ti_Data + 1; continue;
            case TAG_IGNORE:  break;
            case COMPTAG_SrcX:         src_x = (int32)tag->ti_Data; break;
            case COMPTAG_SrcY:         src_y = (int32)tag->ti_Data; break;
            case COMPTAG_SrcWidth:     src_w = (int32)tag->ti_Data; break;
            case COMPTAG_SrcHeight:    src_h = (int32)tag->ti_Data; break;
            case COMPTAG_DestX:        dst_x = (int32)tag->ti_Data; break;
            case COMPTAG_DestY:        dst_y = (int32)tag->ti_Data; break;
            case COMPTAG_DestWidth:    dst_w = (int32)tag->ti_Data; break;
            case COMPTAG_DestHeight:   dst_h = (int32)tag->ti_Data; break;
            case COMPTAG_Flags:        flags = (uint32)tag->ti_Data; break;
            case COMPTAG_Color0:       color0 = (uint32)tag->ti_Data; break;
            case COMPTAG_VertexArray:  has_vertex_array = TRUE; break;
            case COMPTAG_SrcAlphaMask: has_alpha_mask = TRUE; break;
            default: {
                /* Unknown COMPTAG_*: log once per distinct tag value so
                 * we know if AOS4 ever hands us something we don't handle.
                 * Limited to 8 distinct tags so a misbehaving caller can't
                 * spam the log unboundedly. */
                static volatile uint32 ct_warned[8];
                BOOL seen = FALSE;
                for (int i = 0; i < 8; i++)
                    if (ct_warned[i] == tag->ti_Tag) { seen = TRUE; break; }
                if (!seen) {
                    for (int i = 0; i < 8; i++) {
                        if (ct_warned[i] == 0) {
                            ct_warned[i] = tag->ti_Tag;
                            break;
                        }
                    }
                    DCHIP("CompositeTags: unknown tag 0x%08lx data=0x%08lx "
                          "-- ignored", (ULONG)tag->ti_Tag, (ULONG)tag->ti_Data);
                }
                break;
            }
            }
            tstate++;
        }
    }

    /* Default missing dimensions from bitmap sizes (per SDK docs:
     * COMPTAG_SrcWidth/Height "defaults to full width/height") */
    if (gs->IGraphics) {
        if (Source && Source != COMPSRC_SOLIDCOLOR) {
            if (src_w == 0)
                src_w = (int32)gs->IGraphics->GetBitMapAttr(Source, BMA_WIDTH);
            if (src_h == 0)
                src_h = (int32)gs->IGraphics->GetBitMapAttr(Source, BMA_HEIGHT);
        }
        if (Destination) {
            if (dst_w == 0)
                dst_w = (int32)gs->IGraphics->GetBitMapAttr(Destination, BMA_WIDTH);
            if (dst_h == 0)
                dst_h = (int32)gs->IGraphics->GetBitMapAttr(Destination, BMA_HEIGHT);
        }
    }

    /* Unsupported features -> software fallback */
    if (has_vertex_array || has_alpha_mask) {
        g_comp_sw_count++;
        g_comp_total++;
        goto sw_fallback;
    }

    /* With double-buffering, skip HW composite entirely:
     * - The Virgl surface is bound to a fixed resource at creation time.
     *   After buffer swaps, it may target the displayed front buffer.
     * - flush_all overwrites GPU content with board_mem every 5ms anyway,
     *   so the HW result would only be visible for one frame at most.
     * SW composite updates board_mem; flush_all presents it next cycle. */
    if (gs->double_buffer) {
        g_comp_sw_count++;
        g_comp_total++;
        goto sw_fallback;
    }

    /* Determine source type and pixel data */
    BOOL is_solid = (Source == COMPSRC_SOLIDCOLOR);
    const void *src_data = NULL;
    uint32 src_bpr = 0;
    uint32 src_format = RGBFB_A8R8G8B8;

    if (!is_solid && Source) {
        src_data = (const void *)Source->Planes[0];
        src_bpr = Source->BytesPerRow;

        /* Detect actual pixel format from the bitmap */
        if (gs->IGraphics) {
            src_format = (uint32)gs->IGraphics->GetBitMapAttr(Source, BMA_PIXELFORMAT);
        }

        uint32 bpp = chip_format_bpp(src_format);
        if (bpp != 4) {
            g_comp_sw_count++;
            g_comp_total++;
            goto sw_fallback;
        }
    }

    /* Attempt hardware compositing under io_lock to prevent interleaving
     * with flush_all's strip transfers and buffer swaps. */
    {
        struct ExecIFace *IExec = gs->IExec;
        IExec->MutexObtain(gs->io_lock);

        uint32 result = chip_virgl_composite(gs, Operator, Source,
                                               src_data, src_bpr, src_format,
                                               src_x, src_y, src_w, src_h,
                                               dst_x, dst_y, dst_w, dst_h,
                                               flags, color0);

        IExec->MutexRelease(gs->io_lock);

        /* Fall back to software if HW can't handle it */
        if (result == COMPERR_SoftwareFallback || result != COMPERR_Success) {
            g_comp_sw_count++;
            g_comp_total++;
            goto sw_fallback;
        }

        /* Hardware path succeeded -- GPU resource is updated.
         * Also run SW composite to update board_mem (RAM shadow), otherwise
         * the periodic flush task will overwrite GPU result with stale RAM. */
        {
            OrigFunc orig2 = (OrigFunc)g_orig_CompositeTagList;
            orig2(Self, Operator, Source, Destination, tags);
        }

        g_comp_hw_count++;
        g_comp_total++;

        return result;
    }

sw_fallback:
    {
        OrigFunc orig3 = (OrigFunc)g_orig_CompositeTagList;
        return orig3(Self, Operator, Source, Destination, tags);
    }
}

/* -----------------------------------------------------------------------
 * chip_comp_install_hook -- Install CompositeTags hook via SetMethod.
 *
 * Opens graphics.library v54, gets IGraphics, saves the original
 * CompositeTagList method pointer, and replaces it with our hook.
 *
 * Must be called AFTER graphics.library is fully initialized (i.e.,
 * from chip_InitCard_C which runs inside graphics.library's init flow,
 * not from VirtIOGPUBoard which runs before graphics.library).
 * ----------------------------------------------------------------------- */
BOOL chip_comp_install_hook(struct ChipGPUState *gs)
{
    struct ExecIFace *IExec = gs->IExec;

    struct Library *gfxLib = IExec->OpenLibrary("graphics.library", 54);
    if (!gfxLib) {
        DCHIP("comp_hook: cannot open graphics.library v54");
        return FALSE;
    }

    struct Interface *igfx = IExec->GetInterface(gfxLib, "main", 1, NULL);
    if (!igfx) {
        DCHIP("comp_hook: cannot get IGraphics interface");
        IExec->CloseLibrary(gfxLib);
        return FALSE;
    }

    /* vtable is an array of APTR at the interface address */
    APTR *vtable = (APTR *)igfx;
    g_orig_CompositeTagList = vtable[IGFX_COMPOSITETAGLIST_OFFSET / 4];

    IExec->SetMethod(igfx, IGFX_COMPOSITETAGLIST_OFFSET,
                      (APTR)hook_CompositeTagList);

    DCHIP("comp_hook: CompositeTagList hooked at offset %lu (orig=%p)",
          (unsigned long)IGFX_COMPOSITETAGLIST_OFFSET,
          g_orig_CompositeTagList);

    /* Keep gfxLib and igfx open -- the hook needs them alive.
     * DropInterface/CloseLibrary would invalidate the vtable patch.
     * Store in gs for bitmap format queries (GetBitMapAttr). */
    gs->GfxBase = gfxLib;
    gs->IGraphics = (struct GraphicsIFace *)igfx;

    return TRUE;
}

/* -----------------------------------------------------------------------
 * chip_comp_set_dipf_flags -- Advertise DIPF_IS_HWCOMPOSITE on our modes.
 *
 * Enumerates the entire display database via NextDisplayInfo, checks each
 * mode's RTGBoardNum to find modes belonging to our board, and sets the
 * DIPF_IS_HWCOMPOSITE flag in their PropertyFlags.
 *
 * Must be called AFTER graphics.library has built the display database
 * from our ResolutionsList -- i.e., from SetSwitch(TRUE) or later, not
 * from chip_InitCard_C.
 *
 * Safe to call multiple times -- uses a static guard.
 * ----------------------------------------------------------------------- */
void chip_comp_set_dipf_flags(struct ChipGPUState *gs)
{
    static BOOL done = FALSE;
    if (done) return;
    done = TRUE;

    struct ExecIFace *IExec = gs->IExec;

    struct Library *gfxLib = IExec->OpenLibrary("graphics.library", 54);
    if (!gfxLib) {
        DCHIP("comp_dipf: cannot open graphics.library");
        return;
    }
    struct GraphicsIFace *IGraphics = (struct GraphicsIFace *)
        IExec->GetInterface(gfxLib, "main", 1, NULL);
    if (!IGraphics) {
        DCHIP("comp_dipf: cannot get IGraphics");
        IExec->CloseLibrary(gfxLib);
        return;
    }

    /* Get our board number from BoardInfo */
    uint32 our_board = gs->bi ? gs->bi->BoardNum : 0;

    /* Enumerate all display modes in the database */
    uint32 patched = 0;
    uint32 checked = 0;
    uint32 dispID = INVALID_ID;

    while ((dispID = IGraphics->NextDisplayInfo(dispID)) != INVALID_ID) {
        checked++;

        DisplayInfoHandle handle = IGraphics->FindDisplayInfo(dispID);
        if (!handle) continue;

        struct DisplayInfo disp;
        chip_zero(&disp, sizeof(disp));

        uint32 got = IGraphics->GetDisplayInfoData(handle, (APTR)&disp,
                        sizeof(disp), DTAG_DISP, dispID);

        /* OS returns 56 bytes (no reserved[2] terminator), but we only
         * need up to RTGBoardNum at offset 47 -> require 48 bytes min. */
        if (got < 48) continue;

        /* Only patch RTG modes belonging to our board.
         * DIPF_IS_RTG distinguishes RTG modes from native Amiga modes
         * (both have RTGBoardNum=0). */
        if (!(disp.PropertyFlags & DIPF_IS_RTG)) continue;
        if (disp.RTGBoardNum != our_board) continue;

        /* Skip if already set */
        if (disp.PropertyFlags & DIPF_IS_HWCOMPOSITE) {
            patched++;
            continue;
        }

        disp.PropertyFlags |= DIPF_IS_HWCOMPOSITE;

        IGraphics->SetDisplayInfoData(handle, (APTR)&disp,
                        sizeof(disp), DTAG_DISP, dispID);
        patched++;
    }

    DCHIP("comp_dipf: DIPF_IS_HWCOMPOSITE set on %lu/%lu modes (board %lu)",
          (unsigned long)patched, (unsigned long)checked,
          (unsigned long)our_board);

    IExec->DropInterface((struct Interface *)IGraphics);
    IExec->CloseLibrary(gfxLib);
}
