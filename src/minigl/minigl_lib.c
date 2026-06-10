/*
 * minigl_lib.c -- MiniGL shared library for AmigaOS 4.
 *
 * Provides a minimal OpenGL 1.x API backed by Virgl commands submitted
 * to the VirtIO GPU chip driver.  This is a stub/skeleton — GL functions
 * are defined but currently return dummy values or do nothing.
 *
 * The library is built as an AmigaOS 4 shared library with RTF_AUTOINIT,
 * providing both "__library" (manager) and "main" interfaces.
 *
 * Build: ppc-amigaos-gcc -mcrt=newlib -nostartfiles -o minigl.library
 */

#include <exec/exec.h>
#include <exec/types.h>
#include <exec/resident.h>
#include <exec/libraries.h>
#include <exec/interfaces.h>
#include <exec/emulation.h>
#include <dos/dos.h>
#include <proto/exec.h>

#include "minigl/minigl.h"

/* -----------------------------------------------------------------------
 * Library version info
 * ----------------------------------------------------------------------- */
#define LIB_NAME    "minigl.library"
#define LIB_VERSION  1
#define LIB_REVISION 0
#define LIB_IDSTRING "minigl 1.0 (09.03.2026)"

/* -----------------------------------------------------------------------
 * Internal MiniGL context structure
 * ----------------------------------------------------------------------- */
struct MiniGLContext {
    uint32  width, height;
    GLenum  error;

    /* Clear state */
    GLfloat clear_r, clear_g, clear_b, clear_a;
    GLdouble clear_depth;

    /* Current state */
    GLenum  matrix_mode;
    GLenum  shade_model;
    BOOL    depth_test;
    BOOL    blend_enabled;
    BOOL    cull_face;
    BOOL    texture_2d;
    BOOL    scissor_test;
    GLenum  blend_src, blend_dst;
    GLenum  depth_func;
    GLboolean depth_mask;

    /* Immediate mode vertex accumulation */
    GLenum  prim_mode;
    BOOL    in_begin;
    GLfloat cur_color[4];
    GLfloat cur_texcoord[2];
    GLfloat cur_normal[3];

    /* Viewport */
    GLint   vp_x, vp_y;
    GLsizei vp_w, vp_h;

    /* Texture name generator */
    GLuint  next_texture_id;
};

/* Current context (thread-local would be better, but single-threaded for now) */
static struct MiniGLContext *g_current_ctx = NULL;

/* -----------------------------------------------------------------------
 * MiniGL context management
 * ----------------------------------------------------------------------- */

MGLContext mglCreateContext(struct RastPort *rp, uint32 width, uint32 height)
{
    struct ExecIFace *IExec = (struct ExecIFace *)
        ((struct ExecBase *)*(uint32 *)4)->MainInterface;

    struct MiniGLContext *ctx = (struct MiniGLContext *)
        IExec->AllocVecTags(sizeof(struct MiniGLContext),
            AVT_Type, MEMF_PRIVATE, AVT_ClearWithValue, 0, TAG_DONE);
    if (!ctx) return NULL;

    ctx->width  = width;
    ctx->height = height;
    ctx->error  = GL_NO_ERROR;

    /* Defaults */
    ctx->clear_a = 1.0f;
    ctx->clear_depth = 1.0;
    ctx->matrix_mode = GL_MODELVIEW;
    ctx->shade_model = GL_SMOOTH;
    ctx->depth_func  = GL_LESS;
    ctx->depth_mask  = GL_TRUE;
    ctx->blend_src   = GL_ONE;
    ctx->blend_dst   = GL_ZERO;

    ctx->cur_color[0] = 1.0f;
    ctx->cur_color[1] = 1.0f;
    ctx->cur_color[2] = 1.0f;
    ctx->cur_color[3] = 1.0f;
    ctx->cur_normal[2] = 1.0f;  /* default normal = (0,0,1) */

    ctx->vp_w = width;
    ctx->vp_h = height;
    ctx->next_texture_id = 1;

    return (MGLContext)ctx;
}

void mglDeleteContext(MGLContext ctx)
{
    if (!ctx) return;
    struct ExecIFace *IExec = (struct ExecIFace *)
        ((struct ExecBase *)*(uint32 *)4)->MainInterface;
    if (g_current_ctx == (struct MiniGLContext *)ctx)
        g_current_ctx = NULL;
    IExec->FreeVec(ctx);
}

void mglMakeCurrent(MGLContext ctx)
{
    g_current_ctx = (struct MiniGLContext *)ctx;
}

void mglSwapBuffers(MGLContext ctx)
{
    (void)ctx;
    /* TODO: flush Virgl command stream + present */
}

void mglLockDisplay(MGLContext ctx)
{
    (void)ctx;
    /* TODO: acquire rendering lock */
}

void mglUnlockDisplay(MGLContext ctx)
{
    (void)ctx;
    /* TODO: release rendering lock */
}

/* -----------------------------------------------------------------------
 * GL State functions
 * ----------------------------------------------------------------------- */

void glEnable(GLenum cap)
{
    struct MiniGLContext *ctx = g_current_ctx;
    if (!ctx) return;
    switch (cap) {
    case GL_DEPTH_TEST:  ctx->depth_test = TRUE; break;
    case GL_BLEND:       ctx->blend_enabled = TRUE; break;
    case GL_CULL_FACE:   ctx->cull_face = TRUE; break;
    case GL_TEXTURE_2D:  ctx->texture_2d = TRUE; break;
    case GL_SCISSOR_TEST: ctx->scissor_test = TRUE; break;
    default: break;
    }
}

void glDisable(GLenum cap)
{
    struct MiniGLContext *ctx = g_current_ctx;
    if (!ctx) return;
    switch (cap) {
    case GL_DEPTH_TEST:  ctx->depth_test = FALSE; break;
    case GL_BLEND:       ctx->blend_enabled = FALSE; break;
    case GL_CULL_FACE:   ctx->cull_face = FALSE; break;
    case GL_TEXTURE_2D:  ctx->texture_2d = FALSE; break;
    case GL_SCISSOR_TEST: ctx->scissor_test = FALSE; break;
    default: break;
    }
}

void glBlendFunc(GLenum sfactor, GLenum dfactor)
{
    struct MiniGLContext *ctx = g_current_ctx;
    if (!ctx) return;
    ctx->blend_src = sfactor;
    ctx->blend_dst = dfactor;
}

void glDepthFunc(GLenum func)
{
    struct MiniGLContext *ctx = g_current_ctx;
    if (!ctx) return;
    ctx->depth_func = func;
}

void glDepthMask(GLboolean flag)
{
    struct MiniGLContext *ctx = g_current_ctx;
    if (!ctx) return;
    ctx->depth_mask = flag;
}

void glShadeModel(GLenum mode)
{
    struct MiniGLContext *ctx = g_current_ctx;
    if (!ctx) return;
    ctx->shade_model = mode;
}

void glCullFace(GLenum mode)
{
    (void)mode;
    /* TODO: set cull face mode */
}

void glScissor(GLint x, GLint y, GLsizei width, GLsizei height)
{
    (void)x; (void)y; (void)width; (void)height;
    /* TODO: set scissor rect */
}

/* -----------------------------------------------------------------------
 * Clear functions
 * ----------------------------------------------------------------------- */

void glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha)
{
    struct MiniGLContext *ctx = g_current_ctx;
    if (!ctx) return;
    ctx->clear_r = red;
    ctx->clear_g = green;
    ctx->clear_b = blue;
    ctx->clear_a = alpha;
}

void glClearDepth(GLclampd depth)
{
    struct MiniGLContext *ctx = g_current_ctx;
    if (!ctx) return;
    ctx->clear_depth = depth;
}

void glClear(GLbitfield mask)
{
    (void)mask;
    /* TODO: submit Virgl clear command */
}

/* -----------------------------------------------------------------------
 * Viewport / Matrix
 * ----------------------------------------------------------------------- */

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
    struct MiniGLContext *ctx = g_current_ctx;
    if (!ctx) return;
    ctx->vp_x = x;
    ctx->vp_y = y;
    ctx->vp_w = width;
    ctx->vp_h = height;
}

void glMatrixMode(GLenum mode)
{
    struct MiniGLContext *ctx = g_current_ctx;
    if (!ctx) return;
    ctx->matrix_mode = mode;
}

void glLoadIdentity(void)
{
    /* TODO: reset current matrix to identity */
}

void glLoadMatrixf(const GLfloat *m)
{
    (void)m;
    /* TODO: load matrix */
}

void glMultMatrixf(const GLfloat *m)
{
    (void)m;
    /* TODO: multiply current matrix */
}

void glPushMatrix(void)
{
    /* TODO: push matrix stack */
}

void glPopMatrix(void)
{
    /* TODO: pop matrix stack */
}

void glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
             GLdouble zNear, GLdouble zFar)
{
    (void)left; (void)right; (void)bottom; (void)top;
    (void)zNear; (void)zFar;
    /* TODO: multiply by orthographic projection matrix */
}

void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
               GLdouble zNear, GLdouble zFar)
{
    (void)left; (void)right; (void)bottom; (void)top;
    (void)zNear; (void)zFar;
    /* TODO: multiply by perspective projection matrix */
}

void glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{
    (void)x; (void)y; (void)z;
}

void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
    (void)angle; (void)x; (void)y; (void)z;
}

void glScalef(GLfloat x, GLfloat y, GLfloat z)
{
    (void)x; (void)y; (void)z;
}

/* -----------------------------------------------------------------------
 * Immediate mode
 * ----------------------------------------------------------------------- */

void glBegin(GLenum mode)
{
    struct MiniGLContext *ctx = g_current_ctx;
    if (!ctx) return;
    ctx->prim_mode = mode;
    ctx->in_begin = TRUE;
}

void glEnd(void)
{
    struct MiniGLContext *ctx = g_current_ctx;
    if (!ctx) return;
    ctx->in_begin = FALSE;
    /* TODO: flush accumulated vertices via Virgl draw command */
}

void glVertex2f(GLfloat x, GLfloat y)
{
    (void)x; (void)y;
    /* TODO: accumulate vertex */
}

void glVertex3f(GLfloat x, GLfloat y, GLfloat z)
{
    (void)x; (void)y; (void)z;
}

void glVertex4f(GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
    (void)x; (void)y; (void)z; (void)w;
}

void glColor3f(GLfloat r, GLfloat g, GLfloat b)
{
    struct MiniGLContext *ctx = g_current_ctx;
    if (!ctx) return;
    ctx->cur_color[0] = r;
    ctx->cur_color[1] = g;
    ctx->cur_color[2] = b;
    ctx->cur_color[3] = 1.0f;
}

void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
    struct MiniGLContext *ctx = g_current_ctx;
    if (!ctx) return;
    ctx->cur_color[0] = r;
    ctx->cur_color[1] = g;
    ctx->cur_color[2] = b;
    ctx->cur_color[3] = a;
}

void glColor3ub(GLubyte r, GLubyte g, GLubyte b)
{
    glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
}

void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a)
{
    glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

void glTexCoord2f(GLfloat s, GLfloat t)
{
    struct MiniGLContext *ctx = g_current_ctx;
    if (!ctx) return;
    ctx->cur_texcoord[0] = s;
    ctx->cur_texcoord[1] = t;
}

void glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz)
{
    struct MiniGLContext *ctx = g_current_ctx;
    if (!ctx) return;
    ctx->cur_normal[0] = nx;
    ctx->cur_normal[1] = ny;
    ctx->cur_normal[2] = nz;
}

/* -----------------------------------------------------------------------
 * Textures
 * ----------------------------------------------------------------------- */

void glGenTextures(GLsizei n, GLuint *textures)
{
    struct MiniGLContext *ctx = g_current_ctx;
    if (!ctx || !textures) return;
    for (GLsizei i = 0; i < n; i++)
        textures[i] = ctx->next_texture_id++;
}

void glDeleteTextures(GLsizei n, const GLuint *textures)
{
    (void)n; (void)textures;
    /* TODO: free texture resources */
}

void glBindTexture(GLenum target, GLuint texture)
{
    (void)target; (void)texture;
    /* TODO: bind texture for sampling */
}

void glTexImage2D(GLenum target, GLint level, GLint internalformat,
                  GLsizei width, GLsizei height, GLint border,
                  GLenum format, GLenum type, const GLvoid *pixels)
{
    (void)target; (void)level; (void)internalformat;
    (void)width; (void)height; (void)border;
    (void)format; (void)type; (void)pixels;
    /* TODO: upload texture data via Virgl resource_inline_write */
}

void glTexParameteri(GLenum target, GLenum pname, GLint param)
{
    (void)target; (void)pname; (void)param;
}

/* -----------------------------------------------------------------------
 * Queries
 * ----------------------------------------------------------------------- */

GLenum glGetError(void)
{
    struct MiniGLContext *ctx = g_current_ctx;
    if (!ctx) return GL_NO_ERROR;
    GLenum err = ctx->error;
    ctx->error = GL_NO_ERROR;
    return err;
}

const GLubyte *glGetString(GLenum name)
{
    switch (name) {
    case GL_VENDOR:     return (const GLubyte *)"VirtIO GPU / Virgl";
    case GL_RENDERER:   return (const GLubyte *)"VirtIOGPU MiniGL";
    case GL_VERSION:    return (const GLubyte *)"1.0";
    case GL_EXTENSIONS: return (const GLubyte *)"";
    default:            return (const GLubyte *)"";
    }
}

void glGetIntegerv(GLenum pname, GLint *params)
{
    struct MiniGLContext *ctx = g_current_ctx;
    if (!ctx || !params) return;
    switch (pname) {
    case GL_MAX_TEXTURE_SIZE:
        params[0] = 2048;
        break;
    case GL_VIEWPORT:
        params[0] = ctx->vp_x;
        params[1] = ctx->vp_y;
        params[2] = (GLint)ctx->vp_w;
        params[3] = (GLint)ctx->vp_h;
        break;
    default:
        params[0] = 0;
        break;
    }
}

void glGetFloatv(GLenum pname, GLfloat *params)
{
    (void)pname;
    if (params) params[0] = 0.0f;
}

/* -----------------------------------------------------------------------
 * Flush / Finish
 * ----------------------------------------------------------------------- */

void glFlush(void)
{
    /* TODO: submit pending Virgl commands */
}

void glFinish(void)
{
    /* TODO: submit and wait for completion */
}

/* -----------------------------------------------------------------------
 * AmigaOS 4 Shared Library boilerplate
 * ----------------------------------------------------------------------- */

struct MiniGLBase {
    struct Library lib;
    BPTR seglist;
};

/* Manager interface vectors */
static struct MiniGLBase *mgl_lib_Open(struct LibraryManagerInterface *Self, uint32 version)
{
    struct MiniGLBase *base = (struct MiniGLBase *)Self->Data.LibBase;
    base->lib.lib_OpenCnt++;
    return base;
}

static BPTR mgl_lib_Close(struct LibraryManagerInterface *Self)
{
    struct MiniGLBase *base = (struct MiniGLBase *)Self->Data.LibBase;
    if (base->lib.lib_OpenCnt > 0)
        base->lib.lib_OpenCnt--;
    return 0;
}

static BPTR mgl_lib_Expunge(struct LibraryManagerInterface *Self)
{
    (void)Self;
    return 0;  /* Cannot expunge — kickstart resident */
}

static uint32 mgl_lib_Reserved(void)
{
    return 0;
}

/* Library init (RTF_AUTOINIT callback) */
static struct MiniGLBase *mgl_lib_Init(struct MiniGLBase *base,
                                        BPTR seglist,
                                        struct ExecIFace *IExec)
{
    base->seglist = seglist;
    base->lib.lib_Node.ln_Type = NT_LIBRARY;
    base->lib.lib_Node.ln_Pri  = 0;
    base->lib.lib_Node.ln_Name = (STRPTR)LIB_NAME;
    base->lib.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    base->lib.lib_Version  = LIB_VERSION;
    base->lib.lib_Revision = LIB_REVISION;
    base->lib.lib_IdString = (STRPTR)LIB_IDSTRING;

    IExec->DebugPrintF("[minigl] Library initialized\n");
    return base;
}

/* -----------------------------------------------------------------------
 * Manager interface function table
 * ----------------------------------------------------------------------- */
static const APTR mgl_manager_vectors[] = {
    (APTR)mgl_lib_Open,
    (APTR)mgl_lib_Close,
    (APTR)mgl_lib_Expunge,
    (APTR)mgl_lib_Reserved,
    (APTR)-1,
};

static const struct TagItem mgl_manager_tags[] = {
    { MIT_Name,         (ULONG)"__library" },
    { MIT_VectorTable,  (ULONG)mgl_manager_vectors },
    { MIT_Version,      1 },
    { TAG_DONE,         0 }
};

/* -----------------------------------------------------------------------
 * Main interface function table
 *
 * The "main" interface exposes MiniGL functions.  For the stub, we only
 * provide Obtain/Release + the mgl* context management functions.
 * ----------------------------------------------------------------------- */
static uint32 mgl_main_Obtain(struct Interface *Self)
{
    /* Pre-increment: Obtain returns the NEW reference count (>= 1),
     * matching Release's pre-decrement and the exec convention. */
    return ++Self->Data.RefCount;
}

static uint32 mgl_main_Release(struct Interface *Self)
{
    return --Self->Data.RefCount;
}

static const APTR mgl_main_vectors[] = {
    (APTR)mgl_main_Obtain,
    (APTR)mgl_main_Release,
    NULL,  /* Reserved */
    NULL,  /* Reserved */
    (APTR)mglCreateContext,     /* slot 4 */
    (APTR)mglDeleteContext,     /* slot 5 */
    (APTR)mglMakeCurrent,       /* slot 6 */
    (APTR)mglSwapBuffers,       /* slot 7 */
    (APTR)mglLockDisplay,       /* slot 8 */
    (APTR)mglUnlockDisplay,     /* slot 9 */
    (APTR)-1,
};

static const struct TagItem mgl_main_tags[] = {
    { MIT_Name,         (ULONG)"main" },
    { MIT_VectorTable,  (ULONG)mgl_main_vectors },
    { MIT_Version,      1 },
    { TAG_DONE,         0 }
};

/* -----------------------------------------------------------------------
 * Interface list
 * ----------------------------------------------------------------------- */
static const ULONG mgl_interfaces[] = {
    (ULONG)mgl_manager_tags,
    (ULONG)mgl_main_tags,
    0
};

/* -----------------------------------------------------------------------
 * RTF_AUTOINIT create-library tags
 * ----------------------------------------------------------------------- */
static const struct TagItem mgl_create_tags[] = {
    { CLT_DataSize,     sizeof(struct MiniGLBase) },
    { CLT_InitFunc,     (ULONG)mgl_lib_Init },
    { CLT_Interfaces,   (ULONG)mgl_interfaces },
    { TAG_DONE,         0 }
};

/* -----------------------------------------------------------------------
 * Resident tag
 * ----------------------------------------------------------------------- */
static const char mgl_name[] = LIB_NAME;
static const char mgl_id[]   = LIB_IDSTRING;

/* Prevent execution as a shell command.  AmigaOS calls _start with
 * (argstring, arglen, sysbase) — declare them so we can print a
 * diagnostic via DebugPrintF. */
int32 _start(char *argstring __attribute__((unused)),
             int32 arglen __attribute__((unused)),
             struct ExecBase *sysbase)
{
    struct ExecIFace *IExec = (struct ExecIFace *)sysbase->MainInterface;
    IExec->DebugPrintF("%s cannot be executed from a shell.\n", LIB_NAME);
    return 20; /* RETURN_FAIL */
}

static const struct Resident __attribute__((used, section(".text")))
mgl_Resident = {
    RTC_MATCHWORD,
    (struct Resident *)&mgl_Resident,
    (APTR)(&mgl_Resident + 1),
    RTF_AUTOINIT | RTF_NATIVE,
    LIB_VERSION,
    NT_LIBRARY,
    0,              /* pri = 0 (loaded on demand) */
    (STRPTR)mgl_name,
    (STRPTR)mgl_id,
    (APTR)mgl_create_tags
};
