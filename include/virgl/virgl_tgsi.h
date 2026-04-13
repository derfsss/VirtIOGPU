/*
 * virgl_tgsi.h -- TGSI shader constants and binary token format.
 *
 * TGSI (Tungsten Graphics Shader Infrastructure) constants for building
 * shader programs submitted via Virgl CREATE_OBJECT SHADER commands.
 *
 * Two encoding modes:
 *   1. Text mode: TGSI text (e.g. "VERT\nDCL IN[0]\n...") -- parsed by
 *      virglrenderer's tgsi_text_translate(). This is what Mesa's virgl
 *      driver uses. Requires bit 31 set on the first offset word.
 *
 *   2. Binary mode: uint32 token arrays -- bypasses text parser entirely.
 *      More reliable but harder to debug. Not currently used; constants
 *      defined here for future use if text parsing proves unreliable.
 *
 * Reference: Mesa src/gallium/include/pipe/p_shader_tokens.h
 *            Mesa src/gallium/auxiliary/tgsi/tgsi_build.h
 */

#ifndef VIRGL_TGSI_H
#define VIRGL_TGSI_H

/* -----------------------------------------------------------------------
 * TGSI processor types (shader stages)
 * ----------------------------------------------------------------------- */
#define TGSI_PROCESSOR_FRAGMENT     0
#define TGSI_PROCESSOR_VERTEX       1
#define TGSI_PROCESSOR_GEOMETRY     2

/* -----------------------------------------------------------------------
 * TGSI token types
 * ----------------------------------------------------------------------- */
#define TGSI_TOKEN_TYPE_DECLARATION     0
#define TGSI_TOKEN_TYPE_IMMEDIATE       1
#define TGSI_TOKEN_TYPE_INSTRUCTION     2
#define TGSI_TOKEN_TYPE_PROPERTY        3

/* -----------------------------------------------------------------------
 * TGSI register files
 * ----------------------------------------------------------------------- */
#define TGSI_FILE_NULL          0
#define TGSI_FILE_CONSTANT      1
#define TGSI_FILE_INPUT         2
#define TGSI_FILE_OUTPUT        3
#define TGSI_FILE_TEMPORARY     4
#define TGSI_FILE_SAMPLER       5
#define TGSI_FILE_ADDRESS       6
#define TGSI_FILE_IMMEDIATE     7

/* -----------------------------------------------------------------------
 * TGSI semantic names
 * ----------------------------------------------------------------------- */
#define TGSI_SEMANTIC_POSITION      0
#define TGSI_SEMANTIC_COLOR         1
#define TGSI_SEMANTIC_BCOLOR        2
#define TGSI_SEMANTIC_FOG           3
#define TGSI_SEMANTIC_PSIZE         4
#define TGSI_SEMANTIC_GENERIC       5
#define TGSI_SEMANTIC_NORMAL        6
#define TGSI_SEMANTIC_FACE          7
#define TGSI_SEMANTIC_EDGEFLAG      8
#define TGSI_SEMANTIC_PRIMID        9
#define TGSI_SEMANTIC_INSTANCEID    10
#define TGSI_SEMANTIC_VERTEXID      11
#define TGSI_SEMANTIC_STENCIL       12
#define TGSI_SEMANTIC_CLIPDIST      13
#define TGSI_SEMANTIC_CLIPVERTEX    14
#define TGSI_SEMANTIC_TEXCOORD      19

/* -----------------------------------------------------------------------
 * TGSI write mask
 * ----------------------------------------------------------------------- */
#define TGSI_WRITEMASK_X        (1 << 0)
#define TGSI_WRITEMASK_Y        (1 << 1)
#define TGSI_WRITEMASK_Z        (1 << 2)
#define TGSI_WRITEMASK_W        (1 << 3)
#define TGSI_WRITEMASK_XYZW     (0xF)

/* -----------------------------------------------------------------------
 * TGSI swizzle constants
 * ----------------------------------------------------------------------- */
#define TGSI_SWIZZLE_X  0
#define TGSI_SWIZZLE_Y  1
#define TGSI_SWIZZLE_Z  2
#define TGSI_SWIZZLE_W  3

/* -----------------------------------------------------------------------
 * TGSI opcodes (subset needed for compositing shaders)
 *
 * Full list: Mesa src/gallium/include/pipe/p_shader_tokens.h
 * ----------------------------------------------------------------------- */
#define TGSI_OPCODE_ARL     0
#define TGSI_OPCODE_MOV     1
#define TGSI_OPCODE_MUL     7
#define TGSI_OPCODE_ADD     8
#define TGSI_OPCODE_MAD     16
#define TGSI_OPCODE_TEX     52
#define TGSI_OPCODE_END     84      /* NB: verify against virglrenderer */

/* -----------------------------------------------------------------------
 * TGSI texture targets (for TEX instruction)
 * ----------------------------------------------------------------------- */
#define TGSI_TEXTURE_1D         0
#define TGSI_TEXTURE_2D         1
#define TGSI_TEXTURE_3D         2
#define TGSI_TEXTURE_CUBE       3
#define TGSI_TEXTURE_RECT       4
#define TGSI_TEXTURE_SHADOW1D   5
#define TGSI_TEXTURE_SHADOW2D   6
#define TGSI_TEXTURE_SHADOWRECT 7

/* -----------------------------------------------------------------------
 * TGSI interpolation modes (for fragment shader inputs)
 * ----------------------------------------------------------------------- */
#define TGSI_INTERPOLATE_CONSTANT       0
#define TGSI_INTERPOLATE_LINEAR         1
#define TGSI_INTERPOLATE_PERSPECTIVE    2
#define TGSI_INTERPOLATE_COLOR          3

/* -----------------------------------------------------------------------
 * TGSI interpolation location
 * ----------------------------------------------------------------------- */
#define TGSI_INTERPOLATE_LOC_CENTER     0
#define TGSI_INTERPOLATE_LOC_CENTROID   1
#define TGSI_INTERPOLATE_LOC_SAMPLE     2

/* -----------------------------------------------------------------------
 * TGSI shader text strings for Virgl.
 *
 * These are submitted via virgl_cmd_create_shader() as text, which
 * virglrenderer parses via tgsi_text_translate().
 *
 * NOTE: The Virgl protocol requires bit 31 set on the first offset word.
 * Our virgl_cmd_create_shader() handles this automatically.
 * ----------------------------------------------------------------------- */

/* Passthrough VS: position + texcoord/color passthrough.
 *   IN[0]  -> OUT[0] (POSITION)
 *   IN[1]  -> OUT[1] (GENERIC[0], interpolated to FS)
 */
#define TGSI_VS_PASSTHROUGH \
    "VERT\n" \
    "DCL IN[0]\n" \
    "DCL IN[1]\n" \
    "DCL OUT[0], POSITION\n" \
    "DCL OUT[1], GENERIC[0]\n" \
    "  0: MOV OUT[0], IN[0]\n" \
    "  1: MOV OUT[1], IN[1]\n" \
    "  2: END\n"

/* Color FS: pass through interpolated color from VS.
 *   IN[0]  -> OUT[0] (COLOR)
 */
#define TGSI_FS_COLOR \
    "FRAG\n" \
    "DCL IN[0], GENERIC[0], PERSPECTIVE\n" \
    "DCL OUT[0], COLOR\n" \
    "  0: MOV OUT[0], IN[0]\n" \
    "  1: END\n"

/* Texture FS: sample source texture at interpolated texcoord.
 *   IN[0]   = texcoord (GENERIC[0] from VS)
 *   SAMP[0] = source texture sampler
 *   TEX OUT[0], IN[0], SAMP[0], 2D
 *
 * The blend state handles Porter-Duff compositing math.
 */
#define TGSI_FS_TEXTURE \
    "FRAG\n" \
    "DCL IN[0], GENERIC[0], PERSPECTIVE\n" \
    "DCL OUT[0], COLOR\n" \
    "DCL SAMP[0]\n" \
    "  0: TEX OUT[0], IN[0], SAMP[0], 2D\n" \
    "  1: END\n"

#endif /* VIRGL_TGSI_H */
