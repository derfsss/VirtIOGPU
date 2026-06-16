/*
 * warp3d_lib.c -- warp3d.library boilerplate: ROM tag, manager interface,
 * the "main" Warp3DIFace (100 vectors), CLT init tags and _start.
 *
 * warp3d.library (GPL) renders the Warp3D V5 API through the VirtIO-GPU virgl
 * pipeline via the chip's "v3d" transport.  Built -mcrt=newlib -nostartfiles
 * like the chip, with the same newlib discipline.
 */
#include "warp3d_internal.h"
#include <exec/resident.h>
#include <exec/nodes.h>

/* ---- globals ---- */
struct ExecIFace *IExec      = NULL;
struct Library   *g_chipBase = NULL;
struct V3DIFace  *g_IV3D     = NULL;

/* Canonical AmigaOS4 name is "Warp3D.library" (capital W).  exec rejects a
 * disk-loaded library whose romtag name does not case-match the OpenLibrary
 * name, so this MUST match what apps pass (the cow demo opens "Warp3D.library"). */
static const char w3d_name[]  __attribute__((used)) = "Warp3D.library";
static const char w3d_idstr[] __attribute__((used)) =
    "$VER: Warp3D.library 53.20 (16.06.2026)\r\n";

/* ----------------------------------------------------------------------- */
/* Generic stubs (unused-in-M1 methods).  The APTR cast in the vector table  */
/* erases the signature; on the PPC ABI an over-args call is harmless.       */
/* ----------------------------------------------------------------------- */
static uint32 stub_u32(struct Warp3DIFace *Self)  { (void)Self; return 0; }
static void   stub_void(struct Warp3DIFace *Self) { (void)Self; }
static APTR   stub_ptr(struct Warp3DIFace *Self)  { (void)Self; return NULL; }

/* ----------------------------------------------------------------------- */
/* Library manager interface ("__library")                                  */
/* ----------------------------------------------------------------------- */
static uint32 _mgr_Obtain(struct LibraryManagerInterface *Self)
    { return Self->Data.RefCount++; }
static uint32 _mgr_Release(struct LibraryManagerInterface *Self)
    { return Self->Data.RefCount--; }
static struct Library *_mgr_Open(struct LibraryManagerInterface *Self, uint32 version)
{
    struct Library *lib = Self->Data.LibBase;
    (void)version;
    lib->lib_OpenCnt++;
    lib->lib_Flags &= ~LIBF_DELEXP;
    return lib;
}
static BPTR _mgr_Close(struct LibraryManagerInterface *Self)
{
    struct Library *lib = Self->Data.LibBase;
    if (--lib->lib_OpenCnt == 0 && (lib->lib_Flags & LIBF_DELEXP))
        return (BPTR)1;
    return (BPTR)0;
}
static BPTR _mgr_Expunge(struct LibraryManagerInterface *Self)
    { (void)Self; return (BPTR)0; }

static const APTR _mgr_Vectors[] =
{
    (APTR)_mgr_Obtain,
    (APTR)_mgr_Release,
    NULL, NULL,
    (APTR)_mgr_Open,
    (APTR)_mgr_Close,
    (APTR)_mgr_Expunge,
    NULL,
    (APTR)-1
};
static const struct TagItem _mgr_Tags[] =
{
    { MIT_Name,        (Tag)"__library"  },
    { MIT_VectorTable, (Tag)_mgr_Vectors },
    { MIT_Version,     1                 },
    { TAG_DONE,        0                 }
};

/* ----------------------------------------------------------------------- */
/* "main" interface -- Warp3DIFace, 100 vectors in struct order            */
/* ----------------------------------------------------------------------- */
static uint32 _main_Obtain(struct Warp3DIFace *Self)  { return Self->Data.RefCount++; }
static uint32 _main_Release(struct Warp3DIFace *Self) { return Self->Data.RefCount--; }

static const APTR _main_Vectors[] __attribute__((used)) =
{
    (APTR)_main_Obtain,          /* Obtain  */
    (APTR)_main_Release,         /* Release */
    (APTR)stub_void,             /* Expunge */
    (APTR)stub_ptr,              /* Clone   */

    (APTR)w3d_CreateContext,     /*  1 W3D_CreateContext       */
    (APTR)w3d_CreateContextTags, /*  2 W3D_CreateContextTags   */
    (APTR)w3d_DestroyContext,    /*  3 W3D_DestroyContext      */
    (APTR)w3d_GetState,          /*  4 W3D_GetState            */
    (APTR)w3d_SetState,          /*  5 W3D_SetState            */
    (APTR)w3d_CheckDriver,       /*  6 W3D_CheckDriver         */
    (APTR)w3d_LockHardware,      /*  7 W3D_LockHardware        */
    (APTR)w3d_UnLockHardware,    /*  8 W3D_UnLockHardware      */
    (APTR)w3d_WaitIdle,          /*  9 W3D_WaitIdle            */
    (APTR)w3d_CheckIdle,         /* 10 W3D_CheckIdle           */
    (APTR)stub_u32,              /* 11 W3D_Query               */
    (APTR)stub_u32,              /* 12 W3D_GetTexFmtInfo       */
    (APTR)w3d_AllocTexObj,       /* 13 W3D_AllocTexObj         */
    (APTR)w3d_AllocTexObjTags,   /* 14 W3D_AllocTexObjTags     */
    (APTR)w3d_FreeTexObj,        /* 15 W3D_FreeTexObj          */
    (APTR)stub_void,             /* 16 W3D_ReleaseTexture      */
    (APTR)stub_void,             /* 17 W3D_FlushTextures       */
    (APTR)stub_u32,              /* 18 W3D_SetFilter           */
    (APTR)stub_u32,              /* 19 W3D_SetTexEnv           */
    (APTR)stub_u32,              /* 20 W3D_SetWrapMode         */
    (APTR)stub_u32,              /* 21 W3D_UpdateTexImage      */
    (APTR)stub_u32,              /* 22 W3D_UploadTexture       */
    (APTR)stub_u32,              /* 23 W3D_DrawLine            */
    (APTR)stub_u32,              /* 24 W3D_DrawPoint           */
    (APTR)w3d_DrawTriangle,      /* 25 W3D_DrawTriangle        */
    (APTR)stub_u32,              /* 26 W3D_DrawTriFan          */
    (APTR)stub_u32,              /* 27 W3D_DrawTriStrip        */
    (APTR)stub_u32,              /* 28 W3D_SetAlphaMode        */
    (APTR)w3d_SetBlendMode,      /* 29 W3D_SetBlendMode        */
    (APTR)w3d_SetDrawRegion,     /* 30 W3D_SetDrawRegion       */
    (APTR)stub_u32,              /* 31 W3D_SetFogParams        */
    (APTR)stub_u32,              /* 32 W3D_SetColorMask        */
    (APTR)stub_u32,              /* 33 W3D_SetStencilFunc      */
    (APTR)w3d_AllocZBuffer,      /* 34 W3D_AllocZBuffer        */
    (APTR)stub_u32,              /* 35 W3D_FreeZBuffer         */
    (APTR)stub_u32,              /* 36 W3D_ClearZBuffer        */
    (APTR)stub_u32,              /* 37 W3D_ReadZPixel          */
    (APTR)stub_u32,              /* 38 W3D_ReadZSpan           */
    (APTR)stub_u32,              /* 39 W3D_SetZCompareMode     */
    (APTR)stub_u32,              /* 40 W3D_AllocStencilBuffer  */
    (APTR)stub_u32,              /* 41 W3D_ClearStencilBuffer  */
    (APTR)stub_u32,              /* 42 W3D_FillStencilBuffer   */
    (APTR)stub_u32,              /* 43 W3D_FreeStencilBuffer   */
    (APTR)stub_u32,              /* 44 W3D_ReadStencilPixel    */
    (APTR)stub_u32,              /* 45 W3D_ReadStencilSpan     */
    (APTR)stub_u32,              /* 46 W3D_SetLogicOp          */
    (APTR)stub_u32,              /* 47 W3D_Hint                */
    (APTR)stub_u32,              /* 48 W3D_SetDrawRegionWBM    */
    (APTR)stub_u32,              /* 49 W3D_GetDriverState      */
    (APTR)w3d_Flush,             /* 50 W3D_Flush               */
    (APTR)stub_u32,              /* 51 W3D_SetPenMask          */
    (APTR)stub_u32,              /* 52 W3D_SetStencilOp        */
    (APTR)stub_u32,              /* 53 W3D_SetWriteMask        */
    (APTR)stub_u32,              /* 54 W3D_WriteStencilPixel   */
    (APTR)stub_u32,              /* 55 W3D_WriteStencilSpan    */
    (APTR)stub_u32,              /* 56 W3D_WriteZPixel         */
    (APTR)stub_u32,              /* 57 W3D_WriteZSpan          */
    (APTR)stub_u32,              /* 58 W3D_SetCurrentColor     */
    (APTR)stub_u32,              /* 59 W3D_SetCurrentPen       */
    (APTR)stub_u32,              /* 60 W3D_UpdateTexSubImage   */
    (APTR)stub_u32,              /* 61 W3D_FreeAllTexObj       */
    (APTR)stub_u32,              /* 62 W3D_GetDestFmt          */
    (APTR)stub_u32,              /* 63 W3D_DrawLineStrip       */
    (APTR)stub_u32,              /* 64 W3D_DrawLineLoop        */
    (APTR)w3d_GetDrivers,        /* 65 W3D_GetDrivers          */
    (APTR)stub_u32,              /* 66 W3D_QueryDriver         */
    (APTR)stub_u32,              /* 67 W3D_GetDriverTexFmtInfo */
    (APTR)stub_u32,              /* 68 W3D_RequestMode         */
    (APTR)stub_u32,              /* 69 W3D_RequestModeTags     */
    (APTR)stub_void,             /* 70 W3D_SetScissor          */
    (APTR)w3d_FlushFrame,        /* 71 W3D_FlushFrame          */
    (APTR)stub_ptr,              /* 72 W3D_TestMode            */
    (APTR)stub_u32,              /* 73 W3D_SetChromaTestBounds */
    (APTR)w3d_ClearDrawRegion,   /* 74 W3D_ClearDrawRegion     */
    (APTR)stub_u32,              /* 75 W3D_DrawTriangleV       */
    (APTR)stub_u32,              /* 76 W3D_DrawTriFanV         */
    (APTR)stub_u32,              /* 77 W3D_DrawTriStripV       */
    (APTR)stub_ptr,              /* 78 W3D_GetScreenmodeList   */
    (APTR)stub_void,             /* 79 W3D_FreeScreenmodeList  */
    (APTR)stub_u32,              /* 80 W3D_BestModeID          */
    (APTR)stub_u32,              /* 81 W3D_BestModeIDTags      */
    (APTR)w3d_VertexPointer,     /* 82 W3D_VertexPointer       */
    (APTR)stub_u32,              /* 83 W3D_TexCoordPointer     */
    (APTR)w3d_ColorPointer,      /* 84 W3D_ColorPointer        */
    (APTR)w3d_BindTexture,       /* 85 W3D_BindTexture         */
    (APTR)w3d_DrawArray,         /* 86 W3D_DrawArray           */
    (APTR)w3d_DrawElements,      /* 87 W3D_DrawElements        */
    (APTR)stub_void,             /* 88 W3D_SetFrontFace        */
    (APTR)stub_u32,              /* 89 W3D_SetTextureBlend     */
    (APTR)stub_u32,              /* 90 W3D_SetTextureBlendTags */
    (APTR)stub_u32,              /* 91 W3D_SecondaryColorPointer */
    (APTR)stub_u32,              /* 92 W3D_FogCoordPointer     */
    (APTR)w3d_InterleavedArray,  /* 93 W3D_InterleavedArray    */
    (APTR)w3d_ClearBuffers,      /* 94 W3D_ClearBuffers        */
    (APTR)stub_u32,              /* 95 W3D_SetParameter        */
    (APTR)stub_u32,              /* 96 W3D_SetMaxAnisotropy    */
    (APTR)-1                     /* sentinel */
};
static const struct TagItem _main_Tags[] __attribute__((used)) =
{
    { MIT_Name,        (Tag)"main"             },
    { MIT_VectorTable, (Tag)_main_Vectors      },
    { MIT_Version,     1                       },
    { MIT_DataSize,    sizeof(struct Warp3DIFace) },
    { TAG_DONE,        0                       }
};

static const ULONG _w3d_Interfaces[] __attribute__((used)) =
{
    (ULONG)_mgr_Tags,
    (ULONG)_main_Tags,
    0
};

/* ----------------------------------------------------------------------- */
/* Library init                                                             */
/* ----------------------------------------------------------------------- */
static struct Library *_w3d_Init(struct Library *libBase, ULONG seglist,
                                 struct Interface *exec)
{
    (void)seglist;
    if (exec)
        IExec = (struct ExecIFace *)exec;
    libBase->lib_Node.ln_Type = NT_LIBRARY;
    libBase->lib_Node.ln_Name = (char *)w3d_name;
    libBase->lib_Flags        = LIBF_SUMUSED | LIBF_CHANGED;
    libBase->lib_Version      = 53;
    libBase->lib_Revision     = 20;
    libBase->lib_IdString     = (APTR)w3d_idstr;
    return libBase;
}

static const struct TagItem _w3d_InitTags[] __attribute__((used)) =
{
    { CLT_DataSize,      sizeof(struct Library) },
    { CLT_Interfaces,    (Tag)_w3d_Interfaces   },
    { CLT_InitFunc,      (Tag)_w3d_Init         },
    { CLT_NoLegacyIFace, TRUE                   },
    { TAG_DONE,          0                      }
};

static const struct Resident w3d_romtag __attribute__((used, section(".rodata"))) = {
    RTC_MATCHWORD,
    (struct Resident *)&w3d_romtag,
    (APTR)(&w3d_romtag + 1),
    RTF_NATIVE | RTF_AUTOINIT,
    53,
    NT_LIBRARY,
    0,
    (CONST_STRPTR)w3d_name,
    (CONST_STRPTR)w3d_idstr,
    (APTR)_w3d_InitTags,
};

/* No legacy entry point needed -- pure AmigaOS4 interface library. */
__asm__(
    ".section \".text\"\n"
    ".globl _start\n"
    "_start:\n"
    "    li  3, -1\n"
    "    blr\n"
);
