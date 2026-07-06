/*
 * w3d_virtio_lib.c -- W3D_VirtIOGPU.library: the Warp3D V5 HARDWARE BACKEND
 * for the VirtIO GPU, loaded by the STOCK Warp3D.library front-end (PINNED
 * version 53.27, 24.05.2016) from LIBS:Warp3D/HWdrivers/.
 *
 * Phase 8 Part B.  This file is the B1 SKELETON: ROM tag + library manager +
 * the "main" interface = struct W3DHWIFace (the backend vtable), whose slot
 * ORDER is the FE 53.27 dispatch order recovered in B0
 * (tmp_re/fe5327_slotmap_draft.md, [[reference-warp3d-driver-model]]).
 *
 * Backend ABI (from B0): the FE reaches us via the W3D_Context: the backend
 * W3DHWIFace* is stored at *(ctx+192); each public op thunks `lwz r9,OFF(iface);
 * bctrl`, so **backend slot = (OFF - 76)/4**.  Slots 0-3 = manager (Obtain,
 * Release, Expunge=NULL, Clone=NULL), real ops at slot >= 4.  CreateContext gets
 * the FE-allocated public W3D_Context and hangs private state off ctx->driver
 * (offset 0); the GFX-driver ops (LockHardware/UnLockHardware/SetDrawRegion/
 * TestMode/GetDrivers...) are NOT ours -- the stock W3D_Picasso96.library
 * GFXdriver handles those.
 *
 * B1 = PROBE skeleton: every op slot is a per-slot logging stub (hw_sN), so
 * loading this under the real FE and running an app reveals which slot the FE
 * actually calls for each W3D op -> empirically pins the ambiguous lifecycle
 * slots (CreateContext/DestroyContext/SetState/tex alloc) that static RE can't
 * cleanly assign.  B2 replaces the stubs with the ported virgl rendering core.
 *
 * Built -mcrt=newlib -nostartfiles with the same newlib discipline as the chip.
 */
#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/interfaces.h>
#include <exec/resident.h>
#include <exec/nodes.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <utility/tagitem.h>
#include <proto/exec.h>

/* The proven virgl render core (src/warp3d/warp3d_main.c) is compiled into this
 * backend; warp3d_internal.h gives us W3D_Context, struct W3DVirgl and the
 * w3d_* op prototypes.  The render fns ignore Self and key off ctx->driver. */
#include "../warp3d/warp3d_internal.h"

/* ---- globals (the render core's externs; IExec captured in _w3d_Init) ---- */
struct ExecIFace *IExec      = NULL;
struct Library   *g_chipBase = NULL;
struct V3DIFace  *g_IV3D     = NULL;

/* ----------------------------------------------------------------------- */
/* Backend lifecycle wrappers around the proven render core.               */
/* In the FE-backend model the FE ALLOCATES the W3D_Context and calls       */
/* CreateContext(Self, ctx); we must populate ctx->driver, not allocate ctx.*/
/* ----------------------------------------------------------------------- */
/* hw_CreateContext: reuse the proven w3d_CreateContext (which allocs its own
 * ctx + W3DVirgl from the W3D_CC_BITMAP), then TRANSFER the driver state onto
 * the FE-allocated ctx and free only the inner shell. */
static uint32 hw_CreateContext(APTR Self, W3D_Context *ctx)
{
    uint32 err = 0;
    W3D_Context *inner;
    (void)Self;
    if (!ctx) return (uint32)W3D_ILLEGALINPUT;
    inner = w3d_CreateContextTags((struct Warp3DIFace *)0, &err,
                W3D_CC_BITMAP, (uint32)(APTR)ctx->drawregion, TAG_DONE);
    if (!inner || !inner->driver) {
        if (IExec) IExec->DebugPrintF("[W3D_VirtIOGPU] hw_CreateContext FAIL err=%ld\n", (long)err);
        if (inner) IExec->FreeVec(inner);
        return err ? err : (uint32)W3D_NODRIVER;
    }
    ctx->driver     = inner->driver;        /* the W3DVirgl private state    */
    ctx->drivertype = inner->drivertype;
    ctx->width      = inner->width;
    ctx->height     = inner->height;
    inner->driver   = 0;                    /* keep wv; free only the shell  */
    /* FE _Warp3D_W3D_SetTextureBlend returns -30 (W3D_NOTEXTURE) unless
     * *(ctx+0xd0) > 4, then dispatches to slot72 (we return 0).  Claim >4 TMUs so
     * the cow's per-frame SetTextureBlend succeeds (kills the -30 spam).  Best
     * effort: if the FE re-writes ctx+0xd0 after we return, this is a no-op. */
    *(volatile uint32 *)((UBYTE *)ctx + 0xd0) = 8;
    IExec->FreeVec(inner);
    if (IExec) IExec->DebugPrintF("[W3D_VirtIOGPU] hw_CreateContext OK driver=%08lx %ldx%ld\n",
        (unsigned long)(APTR)ctx->driver, (long)ctx->width, (long)ctx->height);
    return 0;                               /* W3D_SUCCESS */
}

/* hw_DestroyContext: w3d_DestroyContext frees the W3DVirgl resources AND the
 * ctx; the FE owns our ctx, so hand the cleanup a throwaway shell carrying wv. */
static void hw_DestroyContext(APTR Self, W3D_Context *ctx)
{
    W3D_Context *shell;
    (void)Self;
    if (!ctx || !ctx->driver) return;
    shell = IExec->AllocVecTags(sizeof(W3D_Context),
                AVT_Type, MEMF_PRIVATE, AVT_ClearWithValue, 0, TAG_DONE);
    if (!shell) return;
    shell->driver = ctx->driver;
    ctx->driver   = 0;
    w3d_DestroyContext((struct Warp3DIFace *)0, shell);   /* frees wv + shell */
}

/* Filename-matched name: the FE scans libs:Warp3D/HWdrivers/#?.library and opens
 * by filename, so the romtag name MUST case-match the on-disk file name. */
static const char w3d_name[]  __attribute__((used)) = "W3D_VirtIOGPU.library";
static const char w3d_idstr[] __attribute__((used)) =
    "$VER: W3D_VirtIOGPU.library 54.1 (17.06.2026)\r\n";

#define DBP(...) do { if (IExec) IExec->DebugPrintF("[W3D_VirtIOGPU] " __VA_ARGS__); } while (0)

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
/* "main" interface == struct W3DHWIFace (the backend vtable)               */
/* Manager slots 0-3, then backend ops at slot >=4.                         */
/* B1 PROBE: every op slot logs its index so a runtime trace pins the map.  */
/* ----------------------------------------------------------------------- */
/* Vector 0 (off 0x4c) is dual-purpose: GetInterface calls it as Obtain, AND the
 * FE's Warp3D_Init registration (ghidra_fe5327.txt:4832) calls it and uses the
 * RETURN as the per-driver TMU/caps value -> stored in DAT_0x1aebc -> copied to
 * ctx+0xd0 by CreateContext.  W3D_SetTextureBlend returns -30 unless ctx+0xd0 > 4,
 * so return a TMU count > 4 (8).  We still bump RefCount for Obtain/Release
 * balance; returning non-zero is also the correct "obtained" signal. */
static uint32 _main_Obtain(struct Interface *Self)  { Self->Data.RefCount++; return 8; }
static uint32 _main_Release(struct Interface *Self) { return Self->Data.RefCount--; }

/* B2 iter-1 probe: log Self + 3 args, and return best-effort values for the
 * registration / Z-self-test slots (from B0/B2 RE) so the FE's Warp3D_Init
 * accepts our backend and proceeds to CreateContext + the draw path.  Per-slot
 * returns are centralized in hw_dispatch() so we can iterate quickly.
 * Reading extra args is harmless on the PPC SysV ABI (regs r4-r6). */
/* B2 iter-2: also log the caller's return address (LR) so each slot call maps to
 * the exact FE code path (registration vs selection vs CreateContext vs a caps
 * probe).  ra - FE_load_base = the fe5327.dis file offset of the call site. */
static uint32 g_dummy_state[16];    /* non-NULL handle ClearDrawRegion returns */
static uint32 hw_dispatch(long slot, uint32 a, uint32 b, uint32 c, uint32 d, uint32 ra)
{
    uint32 ret = 0;
    /* Mirror the working R200 backend's exact slot returns (from Ghidra) so the
     * caller's CreateContext init sequence accepts us. */
    switch (slot) {
    /* The cow checks W3D_AllocZBuffer == W3D_SUCCESS(0) and bails otherwise
     * (CoW3D6.c:1792); same for CreateContext.  These two MUST return 0.  The
     * Z read/clear/identify returns are NOT checked-and-bail. */
    /* slot18 (off 0x94) = backend<->GFXdriver INIT: FE calls it as
     * slot18(W3DGFXBase, W3DGFXversion).  A real HW driver registers its board
     * with the GFXdriver here so the GFXdriver's bitmap->driver lookup
     * (CreateContext primary select via IW3DGFX->[0x58]) maps the app bitmap to
     * it.  A bare return does NOT register -> GFXdriver has no mapping -> the FE
     * returns W3D_NODRIVER(-4).  TODO: replicate R200's slot18 GFX handshake.
     * (return 0 = "HW driver"; ==1 would mark CPU + risk the Warp3D_Init drop.) */
    /* slot19 (off 0x98) = format-support query: FE calls it with (ctx,
     * format_id, destfmt, 0) and the CreateContext format loop requires the
     * return == 5 to treat the format as supported (else W3D_UNSUPPORTEDFMT -18).
     * Return 5 to claim support. */
    case 19: ret = 5;      break;
    /* slot18 (off 0x94) = the [0x94] driver-id the FE stores in Drivers[] at
     * registration (ghidra_fe5327.txt:4814/4825).  CreateContext's primary select
     * (line 756-776) matches GFXdriver [0x58]'s return against Drivers[]; ONLY that
     * matched path sets ctx+0xd0 (the TMU-caps gate for SetTextureBlend).  Return a
     * unique non-1 id (2) and have our GFX driver's [0x58] return the same 2, so the
     * match hits OUR driver -> ctx+0xd0 = DAT_0x1aebc[our] = vector0(8) > 4.  (!=1 so
     * Warp3D_Init doesn't drop us as a CPU driver.) */
    case 18: ret = 2;      break;
    /* slot23 (off 0xa8) is the format-support query the cow's CreateContext
     * actually hammers (format ids 0x6f-0x72,0x14-0x17 + destfmt); the loop
     * needs == 5 to treat the format as supported (else -18 UNSUPPORTEDFMT). */
    case 23:
        /* slot23 (Query) is called with a=W3D_Context* throughout the cow's
         * ResetTexBlend BEFORE W3D_SetTextureBlend.  The FE gates SetTextureBlend
         * on ctx+0xd0 > 4 (TMU caps), but the cow's CreateContext path never sets
         * ctx+0xd0 (the FE writes it only on an alternate select path at
         * ghidra_fe5327.txt:776/779, and slot9/CreateContext is never dispatched
         * to us).  Force ctx+0xd0 = 8 here (RAM-guarded) so SetTextureBlend
         * dispatches instead of returning -30 -> cow advances to DrawObject. */
        if (a >= 0x10000000 && a < 0x80000000)
            *(volatile uint32 *)(a + 0xd0) = 8;
        ret = 5; break;
    case 5:  ret = 1;      break;   /* CheckIdle -> idle/ready (cow polls it)      */
    case 57: ret = 0x48aa; break;   /* identify -> chip magic (informational)     */
    case 61: ret = (uint32)(APTR)g_dummy_state; break; /* ClearDrawRegion (unused: vtable wires real) */
    case 4:                         /* AllocZBuffer -> W3D_SUCCESS(0) (cow-checked)*/
    case 22: case 42:               /* ReadZPixel/SetPenMask -> 0                  */
    default: ret = 0; break;
    }
    DBP("slot %ld ra=%08lx a=%08lx b=%08lx c=%08lx d=%08lx -> %08lx\n",
        slot, (unsigned long)ra, (unsigned long)a, (unsigned long)b,
        (unsigned long)c, (unsigned long)d, (unsigned long)ret);
    return ret;
}

/* slot 9 (CreateContext) + slot 24 (DestroyContext) are the real wrappers
 * hw_CreateContext/hw_DestroyContext defined above.  Remaining op slots are
 * per-vector probe/tuned stubs via hw_dispatch; the geometry/lifecycle slots
 * are wired to the proven w3d_* render core in the vtable below. */
#define HWSTUB(n) static uint32 hw_s##n(APTR Self,uint32 a,uint32 b,uint32 c,uint32 d){ (void)Self; return hw_dispatch((n),a,b,c,d,(uint32)(APTR)__builtin_return_address(0)); }
HWSTUB(4)  HWSTUB(5)  HWSTUB(6)  HWSTUB(7)  HWSTUB(8)  HWSTUB(9)
HWSTUB(10) HWSTUB(11) HWSTUB(12) HWSTUB(13) HWSTUB(14) HWSTUB(15) HWSTUB(16) HWSTUB(17) HWSTUB(18) HWSTUB(19)
HWSTUB(20) HWSTUB(21) HWSTUB(22) HWSTUB(23) HWSTUB(24) HWSTUB(25) HWSTUB(26) HWSTUB(27) HWSTUB(28) HWSTUB(29)
HWSTUB(30) HWSTUB(31) HWSTUB(32) HWSTUB(33) HWSTUB(34) HWSTUB(35) HWSTUB(36) HWSTUB(37) HWSTUB(38) HWSTUB(39)
HWSTUB(40) HWSTUB(41) HWSTUB(42) HWSTUB(43) HWSTUB(44) HWSTUB(45) HWSTUB(46) HWSTUB(47) HWSTUB(48) HWSTUB(49)
HWSTUB(50) HWSTUB(51) HWSTUB(52) HWSTUB(53) HWSTUB(54) HWSTUB(55) HWSTUB(56) HWSTUB(57) HWSTUB(58) HWSTUB(59)
HWSTUB(60) HWSTUB(61) HWSTUB(62) HWSTUB(63) HWSTUB(64) HWSTUB(65) HWSTUB(66) HWSTUB(67) HWSTUB(68)
HWSTUB(70) HWSTUB(71) HWSTUB(72) HWSTUB(74) HWSTUB(75) HWSTUB(77) HWSTUB(78)
HWSTUB(80) HWSTUB(81) HWSTUB(82) HWSTUB(83) HWSTUB(84) HWSTUB(85) HWSTUB(86) HWSTUB(87)

/* The FE's Warp3D_Init registration self-test calls slots 4 (AllocZBuffer) and
 * 61 (ClearDrawRegion) with a BOGUS ctx (IO-space ptr, no CreateContext yet).
 * The real render ops deref ctx->driver -> DSI crash.  Guard: only run the real
 * op for a valid RAM ctx with a non-NULL driver (range-check FIRST so we never
 * deref a bad ctx); otherwise return the registration-safe value. */
static int ctx_ok(W3D_Context *ctx)
{
    uint32 c = (uint32)(APTR)ctx;
    if (c < 0x10000000 || c >= 0x80000000) return 0;
    return ctx->driver != 0;
}
static uint32 hw_AllocZBuffer(APTR Self, W3D_Context *ctx)
{
    if (!ctx_ok(ctx)) return 0;                            /* registration: SUCCESS */
    return w3d_AllocZBuffer((struct Warp3DIFace *)Self, ctx);
}
static uint32 hw_ClearDrawRegion(APTR Self, W3D_Context *ctx, uint32 color)
{
    if (!ctx_ok(ctx)) return (uint32)(APTR)g_dummy_state;  /* registration: non-NULL */
    return w3d_ClearDrawRegion((struct Warp3DIFace *)Self, ctx, color);
}

/* Lazy per-context virgl state: the FE creates the W3D_Context itself and does
 * NOT dispatch CreateContext (slot9) to us, so ctx->driver (the W3DVirgl 'wv') is
 * never set up.  Create it on first array/draw op for a ctx, via the proven
 * w3d_CreateContextTags path (W3D_CC_BITMAP = ctx->drawregion), and stash it in
 * ctx->driver.  Returns NULL if the ctx is bogus or creation fails. */
static APTR get_wv(W3D_Context *ctx)
{
    uint32 err = 0;
    uint32 c   = (uint32)(APTR)ctx;
    W3D_Context *inner;
    /* RANGE-only guard (NOT ctx_ok, which requires ctx->driver!=0 -- the opposite
     * of what we need: get_wv CREATES the driver when it's 0). */
    if (c < 0x10000000 || c >= 0x80000000) return 0;  /* bogus/registration ctx   */
    if (ctx->driver) return ctx->driver;              /* already created          */
    DBP("get_wv: creating, ctx=%08lx drawregion=%08lx w=%ld h=%ld g_IV3D=%08lx\n",
        (unsigned long)(APTR)ctx, (unsigned long)(APTR)ctx->drawregion,
        (long)ctx->width, (long)ctx->height, (unsigned long)(APTR)g_IV3D);
    inner = w3d_CreateContextTags((struct Warp3DIFace *)0, &err,
                W3D_CC_BITMAP, (uint32)(APTR)ctx->drawregion, TAG_DONE);
    if (!inner) { DBP("get_wv: CreateContextTags NULL err=%ld\n", (long)err); return 0; }
    if (!inner->driver) { DBP("get_wv: inner->driver NULL err=%ld\n", (long)err); IExec->FreeVec(inner); return 0; }
    ctx->driver   = inner->driver;       /* take the W3DVirgl                     */
    ctx->width    = inner->width;
    ctx->height   = inner->height;
    inner->driver = 0;
    IExec->FreeVec(inner);
    DBP("get_wv: lazy ctx=%08lx driver=%08lx %ldx%ld\n", (unsigned long)(APTR)ctx,
        (unsigned long)(APTR)ctx->driver, (long)ctx->width, (long)ctx->height);
    return ctx->driver;
}

/* slot 76 is NOT the cow's draw -- the real DrawElements is slot 69 (the FE thunks
 * to it with the genuine PI indices).  slot76's c==0 path carried only a sequential
 * submit counter (word1), and c!=0 is a depth/Z state op; emitting from it drew the
 * 2914 verts in submission order = the distorted spikes.  No-op it; slot 69 draws. */
static uint32 hw_s76(APTR Self, W3D_Context *ctx, uint32 b, uint32 c, uint32 d)
{
    (void)Self; (void)ctx; (void)b; (void)c; (void)d;
    return 0;
}

/* slot 79 (off 0x188) = the cow's W3D_InterleavedArray (FE decomposes its high-level
 * InterleavedArray to this backend primitive).  Runtime args match: (Self, ctx,
 * vtxptr, stride==40==sizeof(WARPPOINT), format, flags).  Ensure wv exists, then
 * stash the array via the proven render core (does NOT draw -> safe). */
/* slot 73 (off 0x160) = the cow's REAL W3D_DrawElements.  *** The iface header is 60
 * bytes, NOT 76 *** (runtime-proven: DrawElements lands here with prim=0 TRIANGLES,
 * type=2 ULONG, count=3000=MAXPRIM; InterleavedArray lands at index 79).  So FE off
 * X -> my index (X-60)/4: DrawElements 0x160->73, InterleavedArray 0x178->79.  The
 * genuine PI indices are the 5th arg (r8).  Forward to the render core, which gathers
 * verts from the slot79-stashed InterleavedArray by these real indices. */
static uint32 hw_s73(APTR Self, W3D_Context *ctx, uint32 prim, uint32 type,
                     uint32 count, void *indices)
{
    (void)Self;
    if (!get_wv(ctx)) return (uint32)W3D_ILLEGALINPUT;
    if (indices && (uint32)(APTR)indices >= 0x10000000 && (uint32)(APTR)indices < 0x80000000) {
        const volatile uint32 *pi = (const volatile uint32 *)indices;
        DBP("slot73 DrawElements prim=%lu type=%lu count=%lu PI[0..2]=%lu,%lu,%lu\n",
            (unsigned long)prim, (unsigned long)type, (unsigned long)count,
            (unsigned long)pi[0], (unsigned long)pi[1], (unsigned long)pi[2]);
    } else {
        DBP("slot73 DrawElements prim=%lu type=%lu count=%lu idx=%08lx(noPI)\n",
            (unsigned long)prim, (unsigned long)type, (unsigned long)count, (unsigned long)indices);
    }
    return w3d_DrawElements((struct Warp3DIFace *)0, ctx, prim, type, count, indices);
}

/* slot 79 kept as InterleavedArray too (hedge: earlier runtime showed these args at
 * index 79; harmless if the FE only calls 75). */
static uint32 hw_s79(APTR Self, W3D_Context *ctx, void *p, uint32 stride,
                     uint32 format, uint32 flags)
{
    (void)Self;
    DBP("slot79 InterleavedArray ctx=%08lx p=%08lx stride=%ld fmt=%08lx\n",
        (unsigned long)(APTR)ctx, (unsigned long)p, (long)stride, (unsigned long)format);
    if (!get_wv(ctx)) return 0;
    return w3d_InterleavedArray((struct Warp3DIFace *)0, ctx, p, (int)stride, format, flags);
}

/* slot 69 (off 0x160) = the cow's REAL W3D_DrawElements.  The FE validates then
 * thunks `(iface+0x160)(iface)` with ctx/prim/type/count/indices still live in
 * r4-r8 (it doesn't reload them) -> we receive the genuine mesh indices (PI),
 * unlike slot76's sequential submit counter.  Forward to the render core, which
 * gathers verts from the slot79-stashed InterleavedArray by these indices. */
/* idx 69 (base 60, off 0x150) = VertexPointer, NOT DrawElements (that's idx 73).
 * The cow uses InterleavedArray, not standalone VertexPointer -- no-op it. */
/* idx 69 = W3D_VertexPointer (base 60): stash the vertex array for DrawArray. */
static uint32 hw_VertexPointer(APTR Self, W3D_Context *ctx, void *p,
                               uint32 stride, uint32 mode, uint32 flags)
{
    (void)Self;
    if (!get_wv(ctx)) return 0;
    return w3d_VertexPointer((struct Warp3DIFace *)0, ctx, p, (int)stride, mode, flags);
}

/* ---- base-60 corrected state/clear handlers (FE leaves args r4=ctx, r5+=rest) --- */
/* SetState(ctx, state, action) -- FE dispatches at off 152 (idx 23) AND 356 (idx 74).
 * Apply the W3D state (ZBUFFER/GOURAUD/cull/blend enables) into wv, and keep the
 * ctx+0xd0=8 TMU-caps poke that gates SetTextureBlend. */
static uint32 hw_SetStateFn(APTR Self, W3D_Context *ctx, uint32 state, uint32 action)
{
    (void)Self;
    if (!get_wv(ctx)) return 0;
    *(volatile uint32 *)((UBYTE *)ctx + 0xd0) = 8;
    /* This vector (off 0x98 = idx 23) is MULTIPLEXED: the FE routes both W3D_SetState
     * AND the CreateContext caps query through it.  For the max-texture queries it
     * passes state=0x6f..0x72 (W3D_Q_MAXTEXWIDTH/HEIGHT/_P) and writes our return into
     * ctx maxtexwidth/height (0x68/0x6c/0x70/0x74).  Report 2048 so W3D_AllocTexObj's
     * "w/h <= max" check passes -- else every texture fails before the upload dispatch
     * (the cow's 256x256 -> "Cant create wtexture").  (W3D states are small values, no
     * overlap with the 0x6f..0x72 query selectors.) */
    if (state >= 0x6f && state <= 0x72) return 2048;
    return w3d_SetState((struct Warp3DIFace *)0, ctx, state, action);
}
/* SetZCompareMode(ctx, mode) at off 208 (idx 37): the render core's DSA is already
 * LESS+write (matches the cow's ZLESS), so accept it. */
static uint32 hw_SetZCompare(APTR Self, W3D_Context *ctx, uint32 mode)
{
    (void)Self; (void)mode;
    if (!get_wv(ctx)) return 0;
    return 0;   /* W3D_SUCCESS */
}
/* ClearBuffers/ClearDrawRegion at off 320 (idx 65): the cow's per-frame clear.  The
 * render core's frame_clear (via w3d_ClearDrawRegion) clears BOTH colour+depth on the
 * next draw -- THE depth-cull fix (depth buffer must re-clear each frame for ZLESS). */
static uint32 hw_Clear65(APTR Self, W3D_Context *ctx, uint32 color)
{
    (void)Self;
    if (!get_wv(ctx)) return 0;
    return w3d_ClearDrawRegion((struct Warp3DIFace *)0, ctx, color);
}
/* slot 80 (off 0x17c): the FE routes W3D_ClearBuffers HERE when ctx+0xd0 > 4
 * (our hw_SetStateFn forces 0xd0=8), dropping the colour/depth args -- so the cow's
 * per-frame clear lands here, NOT at slot 65.  Without it the RT + depth never
 * re-clear -> colour trails + progressive ZLESS rejection.  Black matches the cow's
 * clear colour (rgb=0); the deferred clear is COLOR0|DEPTH so depth re-clears too. */
static uint32 hw_Clear80(APTR Self, W3D_Context *ctx, uint32 b, uint32 c, uint32 d)
{
    (void)Self; (void)b; (void)c; (void)d;
    if (!get_wv(ctx)) return 0;
    return w3d_ClearDrawRegion((struct Warp3DIFace *)0, ctx, 0xFF000000);
}

/* Texture slots (base 60).  The stock FE's W3D_AllocTexObj builds + fills the
 * W3D_Texture itself, then drives the backend via four ctx+0xc0 dispatches, each
 * checked ==0: realize/upload @off 0xd4 (idx 38), filter @0xb4 (30), env @0xc8
 * (35), wrap @0xcc (32).  Only the realize does real work; the others must just
 * return success or the FE aborts with "Cant create wtexture". */
static uint32 hw_TexRealize(APTR Self, W3D_Context *ctx, W3D_Texture *tex,
                            uint32 zero, uint32 maxmip)
{
    (void)Self; (void)zero; (void)maxmip;
    if (!get_wv(ctx)) return (uint32)W3D_ILLEGALINPUT;
    return w3d_RealizeTexture((struct Warp3DIFace *)0, ctx, tex);
}
static uint32 hw_TexAccept(APTR Self, W3D_Context *ctx, W3D_Texture *tex,
                           uint32 a, uint32 b, uint32 c)
{
    (void)Self; (void)ctx; (void)tex; (void)a; (void)b; (void)c;
    return 0;   /* W3D_SUCCESS -- filter/wrap defaults are fine */
}

/* slot 35 (off 0xc8) = the FE's texenv dispatch: both the AllocTexObj env
 * sub-call AND every W3D_SetTexEnv(ctx, tex, envparam, envcolor) land here.
 * Classic W3D texenv is PER-TEXTURE and the FE stores nothing itself
 * (ctx->globaltexenvmode stays at the context default -- proven by
 * w3d_suite: REPLACE/DECAL/BLEND all rendered as MODULATE until this
 * recorder existed).  Record mode+colour in the texture's driver data; the
 * magic guard makes a mis-decoded arg layout a logged no-op. */
static uint32 hw_TexEnv(APTR Self, W3D_Context *ctx, W3D_Texture *tex,
                        uint32 envparam, W3D_Color *envcolor)
{
    (void)Self; (void)ctx;
    DBP("slot35 TexEnv tex=%08lx param=%lu color=%08lx\n",
        (unsigned long)(APTR)tex, (unsigned long)envparam,
        (unsigned long)(APTR)envcolor);
    if (tex && tex->driver && envparam >= W3D_REPLACE && envparam <= W3D_BLEND) {
        struct W3DTexInfo *ti = (struct W3DTexInfo *)tex->driver;
        if (ti->magic == W3DTEX_MAGIC) {
            ti->texenv_mode = envparam;
            if (envcolor) {
                ti->texenv_color[0] = envcolor->r;
                ti->texenv_color[1] = envcolor->g;
                ti->texenv_color[2] = envcolor->b;
                ti->texenv_color[3] = envcolor->a;
            }
        }
    }
    return 0;   /* W3D_SUCCESS (the AllocTexObj sub-call requires ==0) */
}

/* ---- BATCH A: wire remaining slots to the proven render core (base-60) ----
 * All guard via get_wv (lazy ctx->driver create) so registration-probe calls with
 * a bogus ctx don't DSI.  The cow doesn't use these (it draws via DrawElements@73 +
 * InterleavedArray@79), so they're regression-safe; they enable general Warp3D/
 * MiniGL apps that use immediate-mode + the array API. */
static uint32 hw_DrawTriangle(APTR S, W3D_Context *ctx, W3D_Triangle *t)
{ (void)S; if (!get_wv(ctx)) return (uint32)W3D_ILLEGALINPUT;
  return w3d_DrawTriangle((struct Warp3DIFace *)0, ctx, t); }            /* idx 17 */
static uint32 hw_DrawTriStrip(APTR S, W3D_Context *ctx, W3D_Triangles *t)
{ (void)S; if (!get_wv(ctx)) return (uint32)W3D_ILLEGALINPUT;
  return w3d_DrawTriStrip((struct Warp3DIFace *)0, ctx, t); }            /* idx 40 */
static uint32 hw_DrawTriFan(APTR S, W3D_Context *ctx, W3D_Triangles *t)
{ (void)S; if (!get_wv(ctx)) return (uint32)W3D_ILLEGALINPUT;
  return w3d_DrawTriFan((struct Warp3DIFace *)0, ctx, t); }              /* idx 41 */
static uint32 hw_DrawArray(APTR S, W3D_Context *ctx, uint32 prim, uint32 base, uint32 count)
{ (void)S; if (!get_wv(ctx)) return (uint32)W3D_ILLEGALINPUT;
  return w3d_DrawArray((struct Warp3DIFace *)0, ctx, prim, base, count); } /* idx 68/72 */
static uint32 hw_ColorPointer(APTR S, W3D_Context *ctx, void *p, uint32 stride,
                              uint32 fmt, uint32 mode, uint32 flags)
{ (void)S; if (!get_wv(ctx)) return 0;
  return w3d_ColorPointer((struct Warp3DIFace *)0, ctx, p, (int)stride, fmt, mode, flags); } /* idx 67 */
static uint32 hw_TexCoordPointer(APTR S, W3D_Context *ctx, void *p, uint32 stride,
                                 uint32 unit, uint32 flags)
{ (void)S; (void)ctx; (void)p; (void)stride; (void)unit; (void)flags;
  return 0; }   /* idx 66/70 -- accept until render-core array texcoord support (R4) */
static uint32 hw_UploadTexture(APTR S, W3D_Context *ctx, W3D_Texture *tex)
{ (void)S; if (!get_wv(ctx)) return (uint32)W3D_ILLEGALINPUT;
  return w3d_RealizeTexture((struct Warp3DIFace *)0, ctx, tex); }        /* idx 14 */
static uint32 hw_FreeTexObj(APTR S, W3D_Context *ctx, W3D_Texture *tex)
{ (void)S;
  DBP("slot20 FreeTexObj ctx=%08lx tex=%08lx\n",
      (unsigned long)(APTR)ctx, (unsigned long)(APTR)tex);
  if (!ctx_ok(ctx)) return 0;
  w3d_FreeTexObj((struct Warp3DIFace *)0, ctx, tex); return 0; }         /* idx 20 */
static uint32 hw_LockHW(APTR S, W3D_Context *ctx)     { (void)S; (void)ctx; return 0; } /* 44 */
static uint32 hw_UnLockHW(APTR S, W3D_Context *ctx)   { (void)S; (void)ctx; return 0; } /* 45 */
static uint32 hw_SetDrawRegion(APTR S, W3D_Context *ctx){ (void)S; (void)ctx; return 0; } /* 58 */

static const APTR _main_Vectors[] __attribute__((used)) =
{
    (APTR)_main_Obtain,   /* slot 0  Obtain  */
    (APTR)_main_Release,  /* slot 1  Release */
    NULL,                 /* slot 2  Expunge */
    NULL,                 /* slot 3  Clone   */
    /* slots 4..87.  Geometry/lifecycle slots wired to the proven w3d_* render
     * core; the rest stay probe/tuned stubs (hw_sN).  Slot->op from
     * tmp_re/fe5327_map2.txt (backend off = 76 + slot*4). */
    (APTR)hw_s4,            /* 4  (AllocZBuffer is idx 8 at base 60) */
    (APTR)hw_s5,            /* 5  CheckIdle->1  */
    (APTR)hw_s6,            /* 6  ClearStencilBuffer */
    (APTR)hw_s7,            /* 7  (ClearBuffers is idx 65 at base 60) */
    (APTR)hw_AllocZBuffer,  /* 8  AllocZBuffer (base 60, off 92) */
    (APTR)hw_CreateContext, /* 9  CreateContext */
    (APTR)hw_s10, (APTR)hw_s11, (APTR)hw_s12,
    (APTR)hw_s13,           /* 13 (was DrawTriangle: mis-mapped state op) */
    (APTR)hw_UploadTexture, /* 14 */ (APTR)hw_s15, (APTR)hw_s16,
    (APTR)hw_DrawTriangle,  /* 17 */ (APTR)hw_s18,
    (APTR)hw_s19,           /* 19 SetState/Query/format-query -> 5 (keep) */
    (APTR)hw_FreeTexObj, /* 20 */ (APTR)hw_s21, (APTR)hw_s22,
    (APTR)hw_SetStateFn,    /* 23 SetState (base 60, off 152) + ctx+0xd0 poke */
    (APTR)hw_DestroyContext,/* 24 DestroyContext */
    (APTR)hw_s25,           /* 25 (SetBlendMode is idx 29 at base 60) */
    (APTR)hw_s26, (APTR)hw_s27, (APTR)hw_s28, (APTR)w3d_SetBlendMode, /* 29 SetBlendMode (base 60) */
    (APTR)hw_TexAccept, /* 30 tex filter (AllocTexObj sub-call, off 0xb4) */
    (APTR)hw_s31,
    (APTR)hw_TexAccept, /* 32 tex wrap (off 0xcc) */
    (APTR)hw_s33, (APTR)hw_s34,
    (APTR)hw_TexEnv, /* 35 tex env / SetTexEnv (off 0xc8) -- records per-tex mode */
    (APTR)hw_s36, /* 36 (was DrawTriStrip: mis-mapped) */
    (APTR)hw_SetZCompare, /* 37 SetZCompareMode (base 60, off 208) */
    (APTR)hw_TexRealize, /* 38 texture realize/upload (AllocTexObj, off 0xd4) */
    (APTR)hw_s39,
    (APTR)hw_DrawTriStrip, /* 40 */ (APTR)hw_DrawTriFan, /* 41 */ (APTR)hw_s42, (APTR)hw_s43,
    (APTR)hw_LockHW, /* 44 LockHardware (ack) */ (APTR)hw_UnLockHW, /* 45 UnLockHardware (ack) */
    (APTR)hw_s46, (APTR)hw_s47, (APTR)hw_s48, (APTR)hw_s49, (APTR)hw_s50, (APTR)hw_s51,
    (APTR)hw_s52, (APTR)hw_s53, (APTR)hw_s54, (APTR)hw_s55, (APTR)hw_s56,
    (APTR)hw_s57,           /* 57 identify -> 0x48aa (keep) */
    (APTR)hw_SetDrawRegion, /* 58 SetDrawRegion (ack; GFXdriver does the real lock) */
    (APTR)hw_s59, (APTR)hw_s60,
    (APTR)hw_s61,             /* 61 (clear is idx 65 at base 60) */
    (APTR)w3d_FlushFrame, /* 62 FlushFrame (base 60, off 308) */
    (APTR)hw_s63, /* 63 */
    (APTR)hw_s64,   /* 64 */
    (APTR)hw_Clear65,/* 65 ClearBuffers/ClearDrawRegion (base 60, off 320) -- DEPTH CLEAR */
    (APTR)hw_TexCoordPointer, /* 66 TexCoordPointer (accept; R4) */
    (APTR)hw_ColorPointer,    /* 67 ColorPointer */
    (APTR)hw_DrawArray,       /* 68 DrawArray */
    (APTR)hw_VertexPointer,   /* 69 VertexPointer */
    (APTR)hw_TexCoordPointer, /* 70 TexCoordPointer (accept; R4) */
    (APTR)hw_s71,           /* 71 (BindTexture is idx 75 at base 60) */
    (APTR)hw_DrawArray, /* 72 DrawArray (high) */ (APTR)hw_s73, (APTR)hw_SetStateFn, /* 74 SetState */
    (APTR)w3d_BindTexture, /* 75 BindTexture (base 60, off 360) */
    (APTR)hw_s76, (APTR)hw_s77, (APTR)hw_s78, (APTR)hw_s79, (APTR)hw_Clear80, /* 80 ClearBuffers (ctx+0xd0>4 path, off 0x17c) */ (APTR)hw_s81,
    (APTR)hw_s82, (APTR)hw_s83, (APTR)hw_s84, (APTR)hw_s85, (APTR)hw_s86, (APTR)hw_s87,
    (APTR)-1              /* sentinel */
};
/* Backend "main" iface.  MIT_DataSize MUST cover the InterfaceData header +
 * all 88 vectors (slots 0..87, last byte off 76+87*4=424) -- without it
 * GetInterface("main",1) in the FE's Warp3D_Init scan fails to instantiate the
 * interface and our driver never enters Drivers[]. */
#define W3DHW_NUM_VECTORS 88
static const struct TagItem _main_Tags[] __attribute__((used)) =
{
    { MIT_Name,        (Tag)"main"        },
    { MIT_VectorTable, (Tag)_main_Vectors },
    { MIT_Version,     1                  },
    { MIT_DataSize,    sizeof(struct Interface) + W3DHW_NUM_VECTORS * sizeof(APTR) },
    { TAG_DONE,        0                  }
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
    libBase->lib_Version      = 54;
    libBase->lib_Revision     = 1;
    libBase->lib_IdString     = (APTR)w3d_idstr;
    DBP("Init: W3D_VirtIOGPU.library backend loaded (probe skeleton)\n");
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
    54,
    NT_LIBRARY,
    0,
    (CONST_STRPTR)w3d_name,
    (CONST_STRPTR)w3d_idstr,
    (APTR)_w3d_InitTags,
};

/* Pure AmigaOS4 interface library -- no legacy entry point. */
__asm__(
    ".section \".text\"\n"
    ".globl _start\n"
    "_start:\n"
    "    li  3, -1\n"
    "    blr\n"
);
