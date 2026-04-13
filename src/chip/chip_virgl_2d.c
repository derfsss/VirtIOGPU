/*
 * chip_virgl_2d.c -- Virgl 2D acceleration via a 3D rendering context.
 *
 *  OVERVIEW 
 * Replaces the plain 2D VirtIO GPU scanout with a Virgl (OpenGL) 3D
 * rendering context so that Workbench drawing ops can be GPU-accelerated:
 * FillRect via CLEAR, BlitRect via BLIT, compositing via textured quads.
 *
 *  INITIALIZATION FLOW (chip_virgl_init_2d) 
 *  1. Create a 3D context  (CTX_CREATE -> ctx_id)
 *  2. Create scanout resource as a 3D texture (RESOURCE_CREATE_3D) bound
 *     to the same DMA-backed framebuffer pages.  This is how the display
 *     hardware still sees the guest memory while OpenGL can render into it.
 *  3. ATTACH_BACKING to the same DMA list as the 2D resource.
 *  4. SET_SCANOUT -> point display at the 3D resource.
 *  5. Build pipeline state objects:
 *       - Blend states (opaque + alpha-over + 13 Porter-Duff for compositing)
 *       - Depth-stencil-alpha (disabled, pass-through)
 *       - Rasterizer (scissor-enabled, no cull)
 *       - Vertex elements (xy + uv layout for textured quads)
 *       - Shaders (VS passthrough, FS solid-colour, FS sampled-texture)
 *       - Samplers (nearest + linear)
 *       - Vertex buffer resource (PIPE_BUFFER, 256 bytes)
 *       - Surface + framebuffer state (bound to 3D resource)
 *
 *  DOUBLE-BUFFER VARIANT 
 * When double-buffering is enabled, a second 3D resource (virgl_2d_resource2)
 * is created over fb_mem2.  chip_flush_all uses SET_SCANOUT to atomically
 * swap between them, eliminating tearing.
 *
 *  VIRGLRENDERER QUIRK: ONE SHADER PER SUBMISSION 
 * virglrenderer parses CREATE_OBJECT SHADER as a shader definition, but
 * subsequent CREATE_OBJECT SHADER in the SAME command buffer are treated
 * as CONTINUATION packets (appending shader IR) rather than new shaders.
 * To create N distinct shaders, issue N separate virgl_submit() calls.
 * This is why chip_virgl_init_2d has three per-shader submissions instead
 * of batching them.  A cold-boot cost of ~3ms; negligible once running.
 *
 *  CONTEXT POISONING 
 * virglrenderer silently disables a context on protocol errors (bad
 * command length, invalid resource ID, etc.).  Once poisoned, all future
 * commands return ENOENT/EINVAL.  gs->virgl_ctx_error is set on any
 * CMD_SUBMIT_3D failure; the 2D fallback path takes over.  There is no
 * documented way to un-poison a context short of destroying and recreating
 * it, so we simply run in 2D mode for the rest of the session.
 *
 *  SCANOUT FORMAT 
 * Uses PIPE_FORMAT_B8G8R8X8_UNORM which matches the PPC BE byte layout of
 * P96's A8R8G8B8 exactly -- the conversion path is a straight word copy.
 * See chip_flush.c for the rationale.
 *
 *  RESOURCE ID SPACE 
 *   1 = 2D framebuffer (primary)
 *   2 = cursor
 *   3+ = Virgl dynamic resources (pipeline state objects, textures)
 *   9 = 2D framebuffer back-buffer (fixed)
 *
 *  HANDLE ID SPACE 
 * Pipeline state objects use handles in the range 100-119.  Compositing
 * uses 200+ to avoid collision.  See chip_composite.c.
 */

#include "chip/chip_state.h"
#include "virgl/virgl_cmd.h"

/* Object handle IDs for 2D pipeline state */
#define V2D_HANDLE_BLEND         100
#define V2D_HANDLE_BLEND_ALPHA   101
#define V2D_HANDLE_DSA           102
#define V2D_HANDLE_RAST          103
#define V2D_HANDLE_VS            104
#define V2D_HANDLE_FS            105
#define V2D_HANDLE_SURFACE       106
#define V2D_HANDLE_FS_TEX        107
#define V2D_HANDLE_SAMPLER       108
#define V2D_HANDLE_SAMPLER_LINEAR 109
#define V2D_HANDLE_VE            110

/* Vertex buffer size in bytes -- room for 1 quad (6 verts * 32 bytes = 192) */
#define V2D_VBUF_SIZE            256

/* -----------------------------------------------------------------------
 * chip_virgl_init_2d -- Create Virgl 2D acceleration context + 3D scanout.
 *
 * 1. Create 3D context + sub-context
 * 2. Create 3D resource (same dims as framebuffer) -> new scanout
 * 3. Attach backing, set scanout, attach to context
 * 4. Create pipeline state objects (blend, DSA, rasterizer with scissor)
 * 5. Create Virgl surface and bind as framebuffer
 * ----------------------------------------------------------------------- */
BOOL chip_virgl_init_2d(struct ChipGPUState *gs)
{
    gs->virgl_2d_ready = FALSE;

    if (!gs->has_virgl || !gs->virgl_caps_valid || !gs->cmd3d_buf) {
        DCHIP("virgl_init_2d: prerequisites not met");
        return FALSE;
    }

    /* --- Step 1: Create 3D context + sub-context --- */
    uint32 ctx_id = gs->next_ctx_id++;
    gs->virgl_2d_ctx = ctx_id;

    if (!chip_CTXCreate(gs, ctx_id, "virtiogpu-2d", 0)) {
        DCHIP("virgl_init_2d: CTX_CREATE failed");
        return FALSE;
    }

    uint32 cmd_words[512];
    struct VirglCmdBuf cbuf;
    virgl_cmd_init(&cbuf, cmd_words, 512);

    virgl_cmd_create_sub_ctx(&cbuf, 1);
    virgl_cmd_set_sub_ctx(&cbuf, 1);

    if (!virgl_submit(gs, ctx_id, &cbuf)) {
        DCHIP("virgl_init_2d: sub-context creation failed");
        chip_CTXDestroy(gs, ctx_id);
        return FALSE;
    }
    DCHIP("virgl_init_2d: sub-context created OK (ctx=%lu)", ctx_id);

    /* --- Step 2: Create 3D resource for scanout ---
     *
     * SIZE MUST MATCH the active scanout rect, not the max FB
     * dimensions.  QEMU's GL display backend treats the entire 3D
     * resource as the displayed texture and scales the QEMU window to
     * the texture's natural size.  If we create the resource at
     * fb_width x fb_height (4096x4096) the QEMU window opens 4096x4096
     * and our actual visible 1280x800 pixels are crammed into the
     * top-left corner of a much larger window (the rest is cleared
     * texture, displayed as black).
     *
     * Sizing the resource to active_width x active_height makes the GL
     * texture match the visible region.  Mode changes >= the current
     * resource dimensions are handled in chip_SetGC by tearing down
     * the Virgl context and reinitialising via chip_virgl_recover_2d.
     *
     * Backing memory is still the full 64MB fb_mem -- the resource
     * just uses the first active_w*active_h*4 bytes of it. */
    uint32 res_3d = chip_alloc_resource_id(gs);
    uint32 res_w = gs->active_width  ? gs->active_width  : gs->fb_width;
    uint32 res_h = gs->active_height ? gs->active_height : gs->fb_height;
    /* Cap at fb_w/fb_h to never exceed the allocated backing. */
    if (res_w > gs->fb_width)  res_w = gs->fb_width;
    if (res_h > gs->fb_height) res_h = gs->fb_height;
    gs->virgl_2d_res_w = res_w;
    gs->virgl_2d_res_h = res_h;

    if (!chip_ResourceCreate3D(gs, res_3d,
            PIPE_TEXTURE_2D,
            PIPE_FORMAT_B8G8R8X8_UNORM,
            PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_SCANOUT,
            res_w, res_h, 1,  /* width, height, depth */
            1, 0, 0, 0))    /* array_size, last_level, nr_samples, flags */
    {
        DCHIP("virgl_init_2d: RESOURCE_CREATE_3D failed");
        chip_CTXDestroy(gs, ctx_id);
        return FALSE;
    }

    /* Attach the same backing memory (fb_mem DMA pages) */
    if (!chip_ResourceAttachBacking(gs, res_3d,
            gs->fb_dma_list, gs->fb_dma_count))
    {
        DCHIP("virgl_init_2d: ATTACH_BACKING failed for 3D resource");
        chip_CTXDestroy(gs, ctx_id);
        return FALSE;
    }

    /* Switch scanout to the 3D resource (full size of resource = full display) */
    if (!chip_SetScanout(gs, 0, res_3d, 0, 0, res_w, res_h)) {
        DCHIP("virgl_init_2d: SET_SCANOUT failed for 3D resource");
        chip_CTXDestroy(gs, ctx_id);
        return FALSE;
    }

    /* Update the active resource ID -- all flush/scanout ops now use this */
    gs->virgl_2d_resource = res_3d;
    gs->resource_id = res_3d;
    DCHIP("virgl_init_2d: scanout switched to 3D resource %lu (%lux%lu)",
          res_3d, res_w, res_h);

    /* Attach resource to context */
    chip_CTXAttachResource(gs, ctx_id, res_3d);

    /* Create second 3D resource for double-buffering at the same size */
    gs->virgl_2d_resource2 = 0;
    if (gs->double_buffer && gs->fb_mem2) {
        uint32 res_3d_2 = chip_alloc_resource_id(gs);
        if (chip_ResourceCreate3D(gs, res_3d_2,
                PIPE_TEXTURE_2D,
                PIPE_FORMAT_B8G8R8X8_UNORM,
                PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_SCANOUT,
                res_w, res_h, 1, 1, 0, 0, 0) &&
            chip_ResourceAttachBacking(gs, res_3d_2,
                gs->fb_dma_list2, gs->fb_dma_count2))
        {
            chip_CTXAttachResource(gs, ctx_id, res_3d_2);
            gs->virgl_2d_resource2 = res_3d_2;
            DCHIP("virgl_init_2d: second 3D resource %lu for double-buffering", res_3d_2);
        } else {
            DCHIP("virgl_init_2d: second 3D resource failed -- double-buffering disabled");
            gs->double_buffer = FALSE;
        }
    }

    /* --- Step 3: Create pipeline state objects --- */
    virgl_cmd_reset(&cbuf);

    /* Opaque blend */
    virgl_setup_default_blend(&cbuf, V2D_HANDLE_BLEND);
    gs->virgl_2d_blend = V2D_HANDLE_BLEND;

    /* Alpha blend */
    virgl_setup_alpha_blend(&cbuf, V2D_HANDLE_BLEND_ALPHA);
    gs->virgl_2d_blend_alpha = V2D_HANDLE_BLEND_ALPHA;

    /* Depth/stencil: disabled */
    virgl_setup_default_dsa(&cbuf, V2D_HANDLE_DSA);
    gs->virgl_2d_dsa = V2D_HANDLE_DSA;

    /* Rasterizer: fill mode, no culling, SCISSOR ENABLED */
    {
        uint32 s0 = VIRGL_RS_S0_DEPTH_CLIP(1) |
                    VIRGL_RS_S0_CULL_FACE(PIPE_FACE_NONE) |
                    VIRGL_RS_S0_FILL_FRONT(PIPE_POLYGON_MODE_FILL) |
                    VIRGL_RS_S0_FILL_BACK(PIPE_POLYGON_MODE_FILL) |
                    VIRGL_RS_S0_SCISSOR(1);
        DCHIP("virgl_init_2d: rasterizer S0=0x%08lx (scissor bit14=%lu)",
              (unsigned long)s0, (unsigned long)((s0 >> 14) & 1));
        virgl_cmd_create_rasterizer(&cbuf, V2D_HANDLE_RAST,
            s0, 1.0f, 0, 0, 1.0f, 0.0f, 0.0f, 0.0f);
    }
    gs->virgl_2d_rast = V2D_HANDLE_RAST;

    if (!virgl_submit(gs, ctx_id, &cbuf)) {
        DCHIP("virgl_init_2d: pipeline object creation failed");
        chip_CTXDestroy(gs, ctx_id);
        return FALSE;
    }
    DCHIP("virgl_init_2d: blend + DSA + rasterizer created OK");

    /* --- Step 3b: Create shaders ---
     * IMPORTANT: Each shader must be submitted in its OWN command buffer.
     * Virglrenderer's shader parser treats subsequent CREATE_OBJECT SHADER
     * commands in the same buffer as "continuation" packets of the first
     * shader, causing "Got continuation without original long shader" errors
     * that poison the entire context.
     */
    gs->virgl_shaders_ok = TRUE;  /* assume success, clear on failure */

    /* Passthrough vertex shader */
    virgl_cmd_reset(&cbuf);
    virgl_setup_passthrough_vs(&cbuf, V2D_HANDLE_VS);
    if (!virgl_submit(gs, ctx_id, &cbuf)) {
        DCHIP("virgl_init_2d: VS creation failed -- shaders disabled");
        gs->virgl_shaders_ok = FALSE;
    } else {
        gs->virgl_2d_vs = V2D_HANDLE_VS;
        DCHIP("virgl_init_2d: VS=%lu created OK", V2D_HANDLE_VS);
    }

    /* Color fragment shader */
    if (gs->virgl_shaders_ok) {
        virgl_cmd_reset(&cbuf);
        virgl_setup_color_fs(&cbuf, V2D_HANDLE_FS);
        if (!virgl_submit(gs, ctx_id, &cbuf)) {
            DCHIP("virgl_init_2d: FS creation failed -- shaders disabled");
            gs->virgl_shaders_ok = FALSE;
        } else {
            gs->virgl_2d_fs = V2D_HANDLE_FS;
            DCHIP("virgl_init_2d: FS=%lu created OK", V2D_HANDLE_FS);
        }
    }

    /* Texture-sampling fragment shader (for compositing) */
    if (gs->virgl_shaders_ok) {
        virgl_cmd_reset(&cbuf);
        virgl_setup_texture_fs(&cbuf, V2D_HANDLE_FS_TEX);
        if (!virgl_submit(gs, ctx_id, &cbuf)) {
            DCHIP("virgl_init_2d: FS_TEX creation failed -- shaders disabled");
            gs->virgl_shaders_ok = FALSE;
        } else {
            gs->virgl_2d_fs_tex = V2D_HANDLE_FS_TEX;
            DCHIP("virgl_init_2d: FS_TEX=%lu created OK", V2D_HANDLE_FS_TEX);
        }
    }

    if (!gs->virgl_shaders_ok) {
        DCHIP("virgl_init_2d: SHADER CREATION FAILED -- shaders disabled");
        DCHIP("virgl_init_2d: CLEAR/BLIT still work, textured draw unavailable");
        gs->virgl_2d_vs = 0;
        gs->virgl_2d_fs = 0;
        gs->virgl_2d_fs_tex = 0;
        /* Non-fatal: CLEAR and BLIT don't need shaders */
    }

    /* --- Step 3c: Create sampler states (for texture filtering) --- */
    if (gs->virgl_shaders_ok) {
        virgl_cmd_reset(&cbuf);

        /* Nearest-neighbor sampler (no filtering, clamp to edge) */
        {
            uint32 s0 = VIRGL_SAMPLER_S0_WRAP_S(PIPE_TEX_WRAP_CLAMP_TO_EDGE) |
                        VIRGL_SAMPLER_S0_WRAP_T(PIPE_TEX_WRAP_CLAMP_TO_EDGE) |
                        VIRGL_SAMPLER_S0_WRAP_R(PIPE_TEX_WRAP_CLAMP_TO_EDGE) |
                        VIRGL_SAMPLER_S0_MIN_IMG_FILTER(PIPE_TEX_FILTER_NEAREST) |
                        VIRGL_SAMPLER_S0_MIN_MIP_FILTER(PIPE_TEX_MIPFILTER_NONE) |
                        VIRGL_SAMPLER_S0_MAG_IMG_FILTER(PIPE_TEX_FILTER_NEAREST);
            virgl_cmd_create_sampler_state(&cbuf, V2D_HANDLE_SAMPLER,
                s0, 0.0f, 0.0f, 0.0f,     /* lod_bias, min_lod, max_lod */
                0.0f, 0.0f, 0.0f, 0.0f);  /* border RGBA */
        }
        gs->virgl_2d_sampler = V2D_HANDLE_SAMPLER;

        /* Linear (bilinear) sampler for COMPFLAG_SrcFilter */
        {
            uint32 s0 = VIRGL_SAMPLER_S0_WRAP_S(PIPE_TEX_WRAP_CLAMP_TO_EDGE) |
                        VIRGL_SAMPLER_S0_WRAP_T(PIPE_TEX_WRAP_CLAMP_TO_EDGE) |
                        VIRGL_SAMPLER_S0_WRAP_R(PIPE_TEX_WRAP_CLAMP_TO_EDGE) |
                        VIRGL_SAMPLER_S0_MIN_IMG_FILTER(PIPE_TEX_FILTER_LINEAR) |
                        VIRGL_SAMPLER_S0_MIN_MIP_FILTER(PIPE_TEX_MIPFILTER_NONE) |
                        VIRGL_SAMPLER_S0_MAG_IMG_FILTER(PIPE_TEX_FILTER_LINEAR);
            virgl_cmd_create_sampler_state(&cbuf, V2D_HANDLE_SAMPLER_LINEAR,
                s0, 0.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 0.0f);
        }
        gs->virgl_2d_sampler_linear = V2D_HANDLE_SAMPLER_LINEAR;

        if (!virgl_submit(gs, ctx_id, &cbuf)) {
            DCHIP("virgl_init_2d: sampler state creation failed -- textured draw unavailable");
            gs->virgl_samplers_ok = FALSE;
            gs->virgl_2d_sampler = 0;
            gs->virgl_2d_sampler_linear = 0;
        } else {
            DCHIP("virgl_init_2d: sampler states created (nearest=%lu linear=%lu)",
                  V2D_HANDLE_SAMPLER, V2D_HANDLE_SAMPLER_LINEAR);
            gs->virgl_samplers_ok = TRUE;
        }
    } else {
        gs->virgl_samplers_ok = FALSE;
        gs->virgl_2d_sampler = 0;
        gs->virgl_2d_sampler_linear = 0;
    }

    /* --- Step 3d: Create vertex elements (vertex layout) --- */
    if (gs->virgl_shaders_ok) {
        virgl_cmd_reset(&cbuf);

        /* Two elements: position (vec4 float) + attrib (vec4 float) = 32 bytes/vert */
        struct VirglVertexElement ve[2];
        ve[0].src_offset = 0;
        ve[0].instance_divisor = 0;
        ve[0].vertex_buffer_index = 0;
        ve[0].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT;

        ve[1].src_offset = 16;  /* after position: 4 floats * 4 bytes */
        ve[1].instance_divisor = 0;
        ve[1].vertex_buffer_index = 0;
        ve[1].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT;

        virgl_cmd_create_vertex_elements(&cbuf, V2D_HANDLE_VE, 2, ve);

        if (!virgl_submit(gs, ctx_id, &cbuf)) {
            DCHIP("virgl_init_2d: VE creation failed");
            gs->virgl_2d_ve = 0;
        } else {
            gs->virgl_2d_ve = V2D_HANDLE_VE;
            DCHIP("virgl_init_2d: VE=%lu created OK (stride=32)", (unsigned long)V2D_HANDLE_VE);
        }
    }

    /* --- Step 3e: Create PIPE_BUFFER for vertex data --- */
    if (gs->virgl_shaders_ok && gs->virgl_2d_ve) {
        uint32 vbuf_res = chip_alloc_resource_id(gs);

        if (!chip_ResourceCreate3D(gs, vbuf_res,
                PIPE_BUFFER,
                PIPE_FORMAT_R8_UNORM,
                PIPE_BIND_VERTEX_BUFFER,
                V2D_VBUF_SIZE, 1, 1,  /* width=size, height=1, depth=1 */
                1, 0, 0, 0))          /* array_size=1, last_level=0, samples=0, flags=0 */
        {
            DCHIP("virgl_init_2d: vertex buffer resource creation failed");
            gs->virgl_2d_vbuf_res = 0;
        } else {
            chip_CTXAttachResource(gs, ctx_id, vbuf_res);
            gs->virgl_2d_vbuf_res = vbuf_res;
            DCHIP("virgl_init_2d: vbuf resource %lu created OK (%lu bytes)",
                  (unsigned long)vbuf_res, (unsigned long)V2D_VBUF_SIZE);
        }
    }

    /* --- Step 4: Create surface + bind pipeline + set framebuffer --- */
    virgl_cmd_reset(&cbuf);

    virgl_cmd_create_surface(&cbuf, V2D_HANDLE_SURFACE,
        res_3d, PIPE_FORMAT_B8G8R8X8_UNORM, 0, 0);
    gs->virgl_2d_surface = V2D_HANDLE_SURFACE;

    /* Bind pipeline state */
    virgl_cmd_bind_object(&cbuf, VIRGL_OBJECT_BLEND, V2D_HANDLE_BLEND);
    virgl_cmd_bind_object(&cbuf, VIRGL_OBJECT_DSA, V2D_HANDLE_DSA);
    virgl_cmd_bind_object(&cbuf, VIRGL_OBJECT_RASTERIZER, V2D_HANDLE_RAST);

    /* Bind shaders if creation succeeded */
    if (gs->virgl_shaders_ok) {
        virgl_cmd_bind_shader(&cbuf, PIPE_SHADER_VERTEX, V2D_HANDLE_VS);
        virgl_cmd_bind_shader(&cbuf, PIPE_SHADER_FRAGMENT, V2D_HANDLE_FS);
        DCHIP("virgl_init_2d: shaders bound (VS=%lu FS=%lu)", V2D_HANDLE_VS, V2D_HANDLE_FS);
    } else {
        DCHIP("virgl_init_2d: shaders NOT bound (creation failed earlier)");
    }

    /* Bind vertex elements if creation succeeded */
    if (gs->virgl_2d_ve) {
        virgl_cmd_bind_object(&cbuf, VIRGL_OBJECT_VERTEX_ELEMENTS, V2D_HANDLE_VE);
        DCHIP("virgl_init_2d: VE bound (%lu)", (unsigned long)V2D_HANDLE_VE);
    }

    /* Set framebuffer to our surface */
    {
        uint32 surf_handles[1] = { V2D_HANDLE_SURFACE };
        virgl_cmd_set_framebuffer_state(&cbuf, 1, 0, surf_handles);
    }

    /* Set viewport (matches the new resource size, not max FB) */
    virgl_cmd_set_viewport(&cbuf, 0,
        (float)res_w * 0.5f, (float)res_h * 0.5f, 0.5f,
        (float)res_w * 0.5f, (float)res_h * 0.5f, 0.5f);

    /* Set default scissor to full framebuffer */
    virgl_cmd_set_scissor_state(&cbuf, 0, 0, 0, res_w, res_h);

    if (!virgl_submit(gs, ctx_id, &cbuf)) {
        DCHIP("virgl_init_2d: surface/framebuffer bind failed");
        chip_CTXDestroy(gs, ctx_id);
        return FALSE;
    }

    /* --- Step 5: Initial transfer to sync fb_mem -> 3D resource ---
     * Stride for the transfer is fb_stride (the backing memory stride,
     * which is fb_width*4 = 16384 -- the full max-FB stride).  The
     * transfer rect width is the new resource width (= active_width). */
    {
        struct virtio_gpu_box box = {0};
        box.w = res_w;
        box.h = res_h;
        box.d = 1;
        chip_TransferToHost3D(gs, ctx_id, res_3d,
            0, gs->fb_stride, 0, 0, &box);
        chip_ResourceFlush(gs, res_3d, 0, 0, res_w, res_h);
    }

    gs->virgl_2d_ready = TRUE;
    DCHIP("virgl_init_2d: 2D acceleration ready (ctx=%lu res=%lu surf=%lu)",
          ctx_id, res_3d, V2D_HANDLE_SURFACE);

    /* Create compositing blend states (one per Porter-Duff operator) */
    chip_comp_init_blends(gs);

    /* Test quad: 0=off, 1=colored quad, 2=textured quad.
     * Texture pipeline verified working (v53.72). */
    /* if (gs->virgl_2d_ve && gs->virgl_2d_vbuf_res) {
        gs->virgl_test_quad = 2;
        DCHIP("virgl_init_2d: textured test quad ENABLED");
    } */

    return TRUE;
}

/* -----------------------------------------------------------------------
 * chip_virgl_shutdown_2d -- Destroy the Virgl 2D acceleration context.
 * ----------------------------------------------------------------------- */
void chip_virgl_shutdown_2d(struct ChipGPUState *gs)
{
    if (!gs->virgl_2d_ready)
        return;

    uint32 cmd_words[256];
    struct VirglCmdBuf cbuf;
    virgl_cmd_init(&cbuf, cmd_words, 256);

    virgl_cmd_destroy_object(&cbuf, VIRGL_OBJECT_SURFACE, V2D_HANDLE_SURFACE);

    /* Destroy vertex elements if created */
    if (gs->virgl_2d_ve) {
        virgl_cmd_destroy_object(&cbuf, VIRGL_OBJECT_VERTEX_ELEMENTS, V2D_HANDLE_VE);
        DCHIP("virgl_shutdown_2d: VE destroyed");
    }

    /* Destroy shaders if they were created (independent of sampler state) */
    if (gs->virgl_shaders_ok) {
        virgl_cmd_destroy_object(&cbuf, VIRGL_OBJECT_SHADER, V2D_HANDLE_VS);
        virgl_cmd_destroy_object(&cbuf, VIRGL_OBJECT_SHADER, V2D_HANDLE_FS);
        virgl_cmd_destroy_object(&cbuf, VIRGL_OBJECT_SHADER, V2D_HANDLE_FS_TEX);
        DCHIP("virgl_shutdown_2d: shaders destroyed");
    }

    /* Destroy sampler states if they were created (independent of shader state) */
    if (gs->virgl_samplers_ok) {
        virgl_cmd_destroy_object(&cbuf, VIRGL_OBJECT_SAMPLER_STATE, V2D_HANDLE_SAMPLER);
        virgl_cmd_destroy_object(&cbuf, VIRGL_OBJECT_SAMPLER_STATE, V2D_HANDLE_SAMPLER_LINEAR);
        DCHIP("virgl_shutdown_2d: samplers destroyed");
    }

    virgl_cmd_destroy_object(&cbuf, VIRGL_OBJECT_BLEND, V2D_HANDLE_BLEND);
    virgl_cmd_destroy_object(&cbuf, VIRGL_OBJECT_BLEND, V2D_HANDLE_BLEND_ALPHA);
    virgl_cmd_destroy_object(&cbuf, VIRGL_OBJECT_DSA, V2D_HANDLE_DSA);
    virgl_cmd_destroy_object(&cbuf, VIRGL_OBJECT_RASTERIZER, V2D_HANDLE_RAST);

    virgl_submit(gs, gs->virgl_2d_ctx, &cbuf);
    chip_CTXDestroy(gs, gs->virgl_2d_ctx);

    gs->virgl_2d_ready = FALSE;
    DCHIP("virgl_shutdown_2d: context destroyed");
}

/* -----------------------------------------------------------------------
 * chip_virgl_recover_2d -- Recover from a poisoned Virgl context.
 *
 * When a virgl command fails (e.g., BLIT with invalid parameters), the
 * context enters an error state and ALL subsequent commands fail.  This
 * function destroys the old context and recreates everything from scratch.
 *
 * Called from the flush task when virgl_ctx_error is detected.
 * ----------------------------------------------------------------------- */
BOOL chip_virgl_recover_2d(struct ChipGPUState *gs)
{
    DCHIP("virgl_recover: context poisoned, recreating...");

    /* Stop using the virgl path immediately */
    gs->virgl_2d_ready = FALSE;
    gs->virgl_ctx_error = FALSE;  /* clear so CTXDestroy isn't blocked */

    /* Disable scanout entirely before tearing down the 3D resources.
     * This is critical for the GL display path: switching scanout to
     * the 2D fb_resource_id (4096x4096) leaves QEMU's SDL2 GL window
     * sized to 4096x4096 because qemu_console_resize() inside
     * virgl_cmd_set_scanout uses the scanout rect.  When the next
     * SetScanout (in chip_virgl_init_2d) targets a smaller resource,
     * SDL_SetWindowSize on Windows often does *not* shrink the window
     * back -- the window stays at the previous larger size, but the
     * smaller texture is rendered into it stretched, which makes the
     * cursor (rendered at native AmigaOS coords) and the texture
     * (stretched up) misalign.
     *
     * Sending SetScanout(scanout_id=0, resource_id=0, w=0, h=0) hits
     * the `else` branch in QEMU's virgl_cmd_set_scanout which calls
     * dpy_gl_scanout_disable + dpy_gfx_replace_surface(NULL).  This
     * fully tears down the SDL surface so the next SetScanout (with
     * the new smaller dimensions) starts from a clean state and
     * SDL_SetWindowSize honours the new size. */
    chip_SetScanout(gs, 0, 0, 0, 0, 0, 0);
    gs->resource_id = gs->fb_resource_id;

    /* Destroy old context -- VirtIO GPU command, works regardless of virgl state */
    chip_CTXDestroy(gs, gs->virgl_2d_ctx);

    /* Destroy old 3D resources (both buffers) */
    if (gs->virgl_2d_resource) {
        chip_ResourceUnref(gs, gs->virgl_2d_resource);
        gs->virgl_2d_resource = 0;
    }
    if (gs->virgl_2d_resource2) {
        chip_ResourceUnref(gs, gs->virgl_2d_resource2);
        gs->virgl_2d_resource2 = 0;
    }

    /* Destroy vertex buffer resource if it existed */
    if (gs->virgl_2d_vbuf_res) {
        chip_ResourceUnref(gs, gs->virgl_2d_vbuf_res);
        gs->virgl_2d_vbuf_res = 0;
    }

    /* Clear compositing temp state */
    gs->comp_tex_res = 0;
    gs->comp_tex_view = 0;
    gs->comp_next_handle = 0;
    for (uint32 i = 0; i < 15; i++)
        gs->comp_blends[i] = 0;

    /* Reset all virgl object handles */
    gs->virgl_2d_blend = 0;
    gs->virgl_2d_blend_alpha = 0;
    gs->virgl_2d_dsa = 0;
    gs->virgl_2d_rast = 0;
    gs->virgl_2d_vs = 0;
    gs->virgl_2d_fs = 0;
    gs->virgl_2d_fs_tex = 0;
    gs->virgl_2d_surface = 0;
    gs->virgl_2d_sampler = 0;
    gs->virgl_2d_sampler_linear = 0;
    gs->virgl_2d_ve = 0;
    gs->virgl_shaders_ok = FALSE;
    gs->virgl_samplers_ok = FALSE;

    /* Clear error flag before reinit */
    gs->virgl_ctx_error = FALSE;

    /* Switch scanout back to the 2D resource temporarily */
    gs->resource_id = gs->fb_resource_id;

    /* Reinitialize -- creates new context, resource, pipeline */
    BOOL ok = chip_virgl_init_2d(gs);

    if (ok) {
        DCHIP("virgl_recover: context recreated OK (ctx=%lu res=%lu)",
              (unsigned long)gs->virgl_2d_ctx,
              (unsigned long)gs->virgl_2d_resource);
    } else {
        DCHIP("virgl_recover: FAILED -- falling back to 2D-only mode");
        /* Resource ID stays as fb_resource_id (2D resource) */
    }

    return ok;
}

/* -----------------------------------------------------------------------
 * chip_virgl_fill_rect -- GPU-accelerated rectangle fill via Virgl CLEAR.
 *
 * Sets scissor to the fill rect, then issues Virgl CLEAR with the
 * converted fill color.  The GPU fills the rect directly -- no CPU
 * format conversion or TRANSFER_TO_HOST needed.
 *
 * Only handles 32bpp (A8R8G8B8) and 16bpp (R5G6B5) formats.
 * CLUT mode falls back to CPU path.
 * ----------------------------------------------------------------------- */
BOOL chip_virgl_fill_rect(struct ChipGPUState *gs,
                           uint32 x, uint32 y, uint32 w, uint32 h,
                           uint32 color, RGBFTYPE format)
{
    /* Disabled: CLEAR ignores scissor (fills entire resource), and with
     * double-buffering the Virgl surface may target the front (displayed)
     * resource, causing full-screen color flashes.  CPU fill + flush_all
     * handles this correctly via board_mem. */
    (void)x; (void)y; (void)w; (void)h; (void)color; (void)format;
    return FALSE;

    if (!gs->virgl_2d_ready || gs->virgl_ctx_error) return FALSE;

    /* Convert P96 color to float RGBA for Virgl CLEAR */
    float r, g, b, a;

    if (format == RGBFB_CLUT) {
        /* CLUT: look up palette, which is already in B8G8R8X8 format */
        uint32 bgr = gs->palette[color & 0xFF];
        b = (float)((bgr >> 24) & 0xFF) / 255.0f;
        g = (float)((bgr >> 16) & 0xFF) / 255.0f;
        r = (float)((bgr >>  8) & 0xFF) / 255.0f;
        a = 1.0f;
    } else if (chip_format_bpp(format) == 2) {
        /* R5G6B5: extract channels */
        uint16 rgb565 = (uint16)color;
        uint32 rv = ((rgb565 >> 11) & 0x1F);
        uint32 gv = ((rgb565 >>  5) & 0x3F);
        uint32 bv = ((rgb565      ) & 0x1F);
        r = (float)((rv << 3) | (rv >> 2)) / 255.0f;
        g = (float)((gv << 2) | (gv >> 4)) / 255.0f;
        b = (float)((bv << 3) | (bv >> 2)) / 255.0f;
        a = 1.0f;
    } else {
        /* A8R8G8B8: PPC BE uint32 = 0xAARRGGBB */
        r = (float)((color >> 16) & 0xFF) / 255.0f;
        g = (float)((color >>  8) & 0xFF) / 255.0f;
        b = (float)((color      ) & 0xFF) / 255.0f;
        a = (float)((color >> 24) & 0xFF) / 255.0f;
    }

    uint32 cmd_words[64];
    struct VirglCmdBuf cbuf;
    virgl_cmd_init(&cbuf, cmd_words, 64);

    /* Set scissor to the fill rectangle */
    virgl_cmd_set_scissor_state(&cbuf, 0, x, y, x + w, y + h);

    /* CLEAR -- clipped by scissor to fill only the target rect */
    virgl_cmd_clear(&cbuf, PIPE_CLEAR_COLOR0, r, g, b, a, 1.0, 0);

    BOOL ok = virgl_submit(gs, gs->virgl_2d_ctx, &cbuf);

    /* Flush the GPU-side change to the display */
    if (ok) {
        chip_ResourceFlush(gs, gs->resource_id, x, y, w, h);
    }

    return ok;
}

/* -----------------------------------------------------------------------
 * chip_virgl_blit_rect -- GPU-accelerated rectangle copy via Virgl BLIT.
 *
 * Copies a rectangle within the same 3D scanout resource.  Uses Virgl
 * BLIT command which handles overlapping src/dst regions correctly
 * (the GPU resolves copy direction internally).
 * ----------------------------------------------------------------------- */
BOOL chip_virgl_blit_rect(struct ChipGPUState *gs,
                           uint32 sx, uint32 sy, uint32 dx, uint32 dy,
                           uint32 w, uint32 h)
{
    /* Virgl BLIT with src_res == dst_res (same-resource blit) is rejected
     * by virglrenderer 1.1.1 with EINVAL.  OpenGL's glBlitFramebuffer
     * between two FBOs referencing the same texture at the same mip level
     * is undefined behavior, and virglrenderer doesn't implement a temp
     * copy workaround.
     *
     * Fall back to CPU blit (done by caller) + chip_flush to transfer
     * the result to the GPU.  The flush task's 20ms interval provides
     * adequate visual update speed.
     *
     * TODO: Implement DRAW_VBO-based blit (render from sampler view to
     * framebuffer) which avoids the same-resource restriction.
     */
    (void)gs; (void)sx; (void)sy; (void)dx; (void)dy; (void)w; (void)h;
    return FALSE;
}

/* -----------------------------------------------------------------------
 * virgl_upload_vertex_floats -- Upload float vertex data via INLINE_WRITE.
 *
 * Unlike the generic virgl_cmd_resource_inline_write (which packs raw bytes
 * and preserves byte order), this variant emits each float via virgl_emit_float
 * which properly GP32-swaps IEEE 754 bits for the LE host GPU.
 *
 * PPC BE float 1.0f = uint32 0x3F800000, GP32 -> LE 0x3F800000 on host. [OK]
 * The byte-packing path would produce LE 0x0000803F on host. [X]
 * ----------------------------------------------------------------------- */
void virgl_upload_vertex_floats(struct VirglCmdBuf *cbuf,
                                        uint32 res_handle,
                                        const float *data,
                                        uint32 num_floats)
{
    uint32 data_bytes = num_floats * 4;
    uint32 payload_len = 11 + num_floats;

    virgl_emit_dword(cbuf, VIRGL_CMD_HDR(VIRGL_CCMD_RESOURCE_INLINE_WRITE,
                                          0, payload_len));
    virgl_emit_dword(cbuf, res_handle);
    virgl_emit_dword(cbuf, 0);  /* level */
    virgl_emit_dword(cbuf, 0);  /* usage */
    virgl_emit_dword(cbuf, 0);  /* stride (unused for PIPE_BUFFER) */
    virgl_emit_dword(cbuf, 0);  /* layer_stride */
    virgl_emit_dword(cbuf, 0);  /* x = byte offset */
    virgl_emit_dword(cbuf, 0);  /* y */
    virgl_emit_dword(cbuf, 0);  /* z */
    virgl_emit_dword(cbuf, data_bytes);  /* w = size in bytes */
    virgl_emit_dword(cbuf, 1);  /* h */
    virgl_emit_dword(cbuf, 1);  /* d */

    for (uint32 i = 0; i < num_floats; i++)
        virgl_emit_float(cbuf, data[i]);
}

/* -----------------------------------------------------------------------
 * chip_virgl_draw_colored_quad -- Draw a solid colored quad via Virgl.
 *
 * Renders a rectangle at pixel coordinates (x,y,w,h) with the given
 * RGBA color using the passthrough VS + color FS pipeline.
 *
 * Vertex layout: 8 floats per vertex (pos[4] + color[4]), stride=32 bytes.
 * Quad = 2 triangles = 6 vertices.
 *
 * Viewport maps NDC [-1,+1] -> [0,fb_w] / [0,fb_h], so:
 *   x_ndc = 2 * x_pixel / fb_w - 1
 *   y_ndc = 2 * y_pixel / fb_h - 1
 * ----------------------------------------------------------------------- */
BOOL chip_virgl_draw_colored_quad(struct ChipGPUState *gs,
                                   uint32 x, uint32 y, uint32 w, uint32 h,
                                   float r, float g, float b, float a)
{
    if (!gs->virgl_2d_ready || !gs->virgl_shaders_ok)
        return FALSE;
    if (!gs->virgl_2d_vbuf_res || !gs->virgl_2d_ve)
        return FALSE;

    uint32 ctx_id = gs->virgl_2d_ctx;
    uint32 fb_w = gs->fb_width;
    uint32 fb_h = gs->fb_height;

    /* Convert pixel coords to NDC.  Viewport maps [-1,+1] -> [0,fb_w/h]. */
    float x0_ndc = 2.0f * (float)x / (float)fb_w - 1.0f;
    float y0_ndc = 2.0f * (float)y / (float)fb_h - 1.0f;
    float x1_ndc = 2.0f * (float)(x + w) / (float)fb_w - 1.0f;
    float y1_ndc = 2.0f * (float)(y + h) / (float)fb_h - 1.0f;

    /* 6 vertices x 8 floats (pos[4] + color[4]) = 48 floats = 192 bytes.
     * Two triangles: (v0,v1,v2) and (v0,v2,v3) in CCW order. */
    float verts[48] = {
        /* Triangle 1: top-left, top-right, bottom-right */
        x0_ndc, y0_ndc, 0.0f, 1.0f,  r, g, b, a,
        x1_ndc, y0_ndc, 0.0f, 1.0f,  r, g, b, a,
        x1_ndc, y1_ndc, 0.0f, 1.0f,  r, g, b, a,
        /* Triangle 2: top-left, bottom-right, bottom-left */
        x0_ndc, y0_ndc, 0.0f, 1.0f,  r, g, b, a,
        x1_ndc, y1_ndc, 0.0f, 1.0f,  r, g, b, a,
        x0_ndc, y1_ndc, 0.0f, 1.0f,  r, g, b, a,
    };

    /* Command buffer: INLINE_WRITE(11+48=59) + SET_VERTEX_BUFFERS(1+3=4) +
     * DRAW_VBO(1+12=13) = 76 words.  Use 96 for margin. */
    uint32 cmd_words[96];
    struct VirglCmdBuf cbuf;
    virgl_cmd_init(&cbuf, cmd_words, 96);

    /* Upload vertex data to the PIPE_BUFFER resource */
    virgl_upload_vertex_floats(&cbuf, gs->virgl_2d_vbuf_res, verts, 48);

    /* Bind vertex buffer: stride=32 (8 floats x 4 bytes), offset=0 */
    {
        struct VirglVertexBuffer vb;
        vb.stride = 32;
        vb.buffer_offset = 0;
        vb.res_handle = gs->virgl_2d_vbuf_res;
        virgl_cmd_set_vertex_buffers(&cbuf, 1, &vb);
    }

    /* Draw: 6 vertices, TRIANGLES, no indexing */
    virgl_cmd_draw_vbo(&cbuf,
        0, 6, PIPE_PRIM_TRIANGLES,  /* start, count, mode */
        0, 1,                        /* indexed=0, instance_count=1 */
        0, 0,                        /* index_bias, start_instance */
        0, 0,                        /* primitive_restart, restart_index */
        0, 5,                        /* min_index, max_index */
        0);                          /* cso_handle (0 = use bound state) */

    static uint32 dump_count = 0;
    if (dump_count < 3) {
        dump_count++;
        DCHIP("draw_colored_quad: DRAW_VBO submit %lu dwords, vbuf_res=%lu",
              (unsigned long)cbuf.dwords, (unsigned long)gs->virgl_2d_vbuf_res);
        DCHIP("  NDC: x0=%.4f y0=%.4f x1=%.4f y1=%.4f",
              x0_ndc, y0_ndc, x1_ndc, y1_ndc);
    }

    BOOL ok = virgl_submit(gs, ctx_id, &cbuf);

    if (ok) {
        chip_ResourceFlush(gs, gs->resource_id, x, y, w, h);
        DCHIP_V("draw_colored_quad: OK (%lu,%lu %lux%lu)",
              (unsigned long)x, (unsigned long)y,
              (unsigned long)w, (unsigned long)h);
    } else {
        DCHIP("draw_colored_quad: SUBMIT FAILED");
    }

    return ok;
}
