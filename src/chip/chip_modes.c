/*
 * chip_modes.c -- Screen mode table and mode registration.
 *
 * Contains the SVGA mode timing table and chip_register_modes() which
 * populates the ResolutionsList with Resolution + ModeInfo entries.
 */

#include "chip/chip_state.h"
#include <graphics/displayinfo.h>
#include <graphics/modeid.h>
#include <proto/graphics.h>

/* Standard SVGA/HD mode table with VESA CRTC timing.
 * VirtIO GPU is virtual -- any resolution is supported.
 * EDID callback at bi+1714 provides monitor capability data;
 * these table entries provide the actual Resolution+ModeInfo objects
 * that appear in ScreenMode preferences. */
const struct ChipModeEntry chip_modes[] = {
    /* idx 0 reserved -- graphics.library treats return 0 as failure */
    {    0,    0,         0,    0,  0,  0,    0,  0, 0, 0 },
    /* 640x480@60 -- VESA DMT */
    {  640,  480,  25175000,  800, 16, 96,  525, 10, 2, GMF_HPOLARITY | GMF_VPOLARITY },
    /* 800x600@60 -- VESA DMT */
    {  800,  600,  40000000, 1056, 40,128,  628,  1, 4, GMF_HPOLARITY | GMF_VPOLARITY },
    /* 1024x768@60 -- VESA DMT */
    { 1024,  768,  65000000, 1344, 24,136,  806,  3, 6, GMF_HPOLARITY | GMF_VPOLARITY },
    /* 1280x720@60 -- CEA-861 (720p) */
    { 1280,  720,  74250000, 1650, 110, 40,  750,  5, 5, 0 },
    /* 1280x800@60 -- CVT reduced blanking */
    { 1280,  800,  83500000, 1440, 48, 32,  823,  3, 6, GMF_HPOLARITY | GMF_VPOLARITY },
    /* 1280x1024@60 -- VESA DMT */
    { 1280, 1024, 108000000, 1688, 48,112, 1066,  1, 3, 0 },
    /* 1440x900@60 -- CVT reduced blanking */
    { 1440,  900,  88750000, 1600, 48, 32,  926,  3, 6, GMF_HPOLARITY | GMF_VPOLARITY },
    /* 1600x900@60 -- CVT reduced blanking */
    { 1600,  900, 108000000, 1800, 24, 80, 1000,  1, 3, 0 },
    /* 1600x1200@60 -- VESA DMT */
    { 1600, 1200, 162000000, 2160, 64,192, 1250,  1, 3, 0 },
    /* 1920x1080@60 -- CEA-861 (1080p) */
    { 1920, 1080, 148500000, 2200, 88, 44, 1125,  4, 5, 0 },
    /* 1920x1200@60 -- CVT reduced blanking */
    { 1920, 1200, 154000000, 2080, 48, 32, 1235,  3, 6, GMF_HPOLARITY | GMF_VPOLARITY },
    /* 2560x1440@60 -- CVT reduced blanking */
    { 2560, 1440, 241500000, 2720, 48, 32, 1478,  3, 5, GMF_HPOLARITY | GMF_VPOLARITY },
    /* 3840x2160@60 -- CEA-861 (4K UHD) */
    { 3840, 2160, 594000000, 4400, 176, 88, 2250, 8, 10, 0 },
};
const ULONG chip_num_modes = sizeof(chip_modes) / sizeof(chip_modes[0]);

/* Mode-class depth table (3 ModeInfo entries per Resolution: CHUNKY,
 * HICOLOR, TRUEALPHA -- same set used by both initial registration and
 * hotplug additions). */
static const struct {
    int   mode_class;   /* index into Resolution::Modes[] */
    UBYTE depth;        /* bits per pixel */
} chip_depth_table[] = {
    { 1,  8 },          /* CHUNKY  -- 8bpp CLUT */
    { 2, 16 },          /* HICOLOR -- 16bpp R5G6B5 */
    { 4, 32 },          /* TRUEALPHA -- 32bpp ARGB */
};
#define CHIP_NUM_DEPTHS (sizeof(chip_depth_table) / sizeof(chip_depth_table[0]))

/* Walk the ResolutionsList and report whether a Resolution with the
 * given dimensions already exists.  Used to skip duplicates when
 * adding a mode at hotplug time.
 *
 * Uses IExec->GetHead/GetSucc -- AOS4 SDK preferred over raw mln_Succ
 * pointer walks (see Exec_Lists_and_Queues wiki).  bi->ResolutionsList
 * is a MinList per the canonical Picasso96 layout, but Resolution
 * itself starts with a full struct Node, so casting through (List *)
 * and (Node *) is well-defined: List/MinList share the same head/tail
 * field offsets and Node/MinNode share the same successor offset. */
static BOOL chip_resolution_exists(struct BoardInfo *bi, UWORD w, UWORD h)
{
    struct ExecIFace *IExec = ((struct ChipGPUState *)g_chip_state)->IExec;
    struct Node *node;
    for (node = IExec->GetHead((struct List *)&bi->ResolutionsList);
         node != NULL;
         node = IExec->GetSucc(node)) {
        struct Resolution *r = (struct Resolution *)node;
        if (r->Width == w && r->Height == h) return TRUE;
    }
    return FALSE;
}

/* -----------------------------------------------------------------------
 * chip_add_mode -- build and register one Resolution + its ModeInfos.
 *
 * Used by chip_register_modes for the initial fill and by
 * chip_handle_display_change for hotplug additions.  Caller supplies
 * dimensions, CRTC timing, and a slot index used in the DisplayID
 * encoding.  Returns TRUE on success (Resolution added to the list).
 *
 * The CRTC timing fields can be all zero -- CVT-RB defaults will be
 * synthesised in that case (the same path used for the EDID-derived
 * mode in the original chip_register_modes).
 * ----------------------------------------------------------------------- */
BOOL chip_add_mode(struct ChipGPUState *gs, UWORD mw, UWORD mh,
                   UWORD hor_total, UWORD hor_sync_start, UWORD hor_sync_size,
                   UWORD ver_total, UWORD ver_sync_start, UWORD ver_sync_size,
                   ULONG mode_clock, UBYTE mode_flags, ULONG slot_idx)
{
    struct ExecIFace *IExec = gs->IExec;
    struct BoardInfo *bi = gs->bi;
    if (!bi) {
        DCHIP("chip_add_mode: gs->bi NULL -- chip_InitCard never published it?");
        return FALSE;
    }
    if (mw == 0 || mh == 0) {
        DCHIP("chip_add_mode: rejecting %ux%u (zero dimension)",
              (unsigned)mw, (unsigned)mh);
        return FALSE;
    }

    if (chip_resolution_exists(bi, mw, mh)) {
        DCHIP_V("chip_add_mode: %ux%u already registered, skipping",
                (unsigned)mw, (unsigned)mh);
        return TRUE;
    }

    /* Synthesise CVT-RB timing if the caller didn't supply one. */
    if (hor_total == 0 || ver_total == 0) {
        hor_total      = mw + 160;
        hor_sync_start = 48;
        hor_sync_size  = 32;
        ver_total      = mh + 23;
        ver_sync_start = 3;
        ver_sync_size  = 6;
        mode_clock     = (ULONG)hor_total * (ULONG)ver_total * 60;
        mode_flags     = GMF_HPOLARITY | GMF_VPOLARITY;
    }

    struct Resolution *res = (struct Resolution *)IExec->AllocVecTags(
        sizeof(struct Resolution),
        AVT_Type, MEMF_SHARED,
        AVT_ClearWithValue, 0,
        TAG_DONE);
    if (!res) {
        DCHIP("chip_add_mode: alloc Resolution %ux%u failed", (unsigned)mw, (unsigned)mh);
        return FALSE;
    }

    UWORD board_num = bi->BoardNum;
    res->P96ID[0] = 'R'; res->P96ID[1] = 'T'; res->P96ID[2] = 'G';
    res->P96ID[3] = '-'; res->P96ID[4] = '0' + (board_num % 10);
    res->P96ID[5] = ':';

    /* Build "VIO:WxH" name without SNPrintf -- runs at chip-init time
     * when stdio paths aren't fully ready. */
    {
        char *p = res->Name;
        const char *prefix = "VIO:";
        while (*prefix) *p++ = *prefix++;
        UWORD val = mw;
        char digits[6]; int nd = 0;
        do { digits[nd++] = '0' + (val % 10); val /= 10; } while (val > 0);
        for (int i = nd - 1; i >= 0; i--) *p++ = digits[i];
        *p++ = 'x';
        val = mh; nd = 0;
        do { digits[nd++] = '0' + (val % 10); val /= 10; } while (val > 0);
        for (int i = nd - 1; i >= 0; i--) *p++ = digits[i];
        *p = '\0';
    }

    res->Node.ln_Name = res->P96ID;
    res->Node.ln_Type = 0;
    res->Node.ln_Pri  = 0;
    res->DisplayID = 0x00050000 | ((slot_idx & 0xFF) << 8);
    res->Width  = mw;
    res->Height = mh;
    res->Flags  = P96F_FAMILY;
    res->BoardInfo = bi;
    res->Reserved = 0;

    ULONG modeinfos_created = 0;
    for (ULONG d = 0; d < CHIP_NUM_DEPTHS; d++) {
        int mc = chip_depth_table[d].mode_class;
        struct ModeInfo *mi = (struct ModeInfo *)IExec->AllocVecTags(
            sizeof(struct ModeInfo),
            AVT_Type, MEMF_SHARED,
            AVT_ClearWithValue, 0,
            TAG_DONE);
        if (!mi) {
            DCHIP("chip_add_mode: alloc ModeInfo %ux%u d=%u failed",
                  (unsigned)mw, (unsigned)mh, (unsigned)chip_depth_table[d].depth);
            continue;
        }
        modeinfos_created++;
        mi->Width  = mw;
        mi->Height = mh;
        mi->Depth  = chip_depth_table[d].depth;
        mi->Flags  = mode_flags;
        mi->HorTotal     = hor_total;
        mi->HorBlankSize = hor_total - mw;
        mi->HorSyncStart = hor_sync_start;
        mi->HorSyncSize  = hor_sync_size;
        mi->VerTotal     = ver_total;
        mi->VerBlankSize = ver_total - mh;
        mi->VerSyncStart = ver_sync_start;
        mi->VerSyncSize  = ver_sync_size;
        mi->PixelClock = mode_clock;
        mi->clock      = (UBYTE)(slot_idx < chip_num_modes ? slot_idx : 1);
        mi->clock_div  = 1;
        mi->Node.ln_Name = NULL;
        res->Modes[mc] = mi;
    }

    if (modeinfos_created == 0) {
        IExec->FreeVec(res);
        DCHIP("chip_add_mode: %ux%u dropped -- no ModeInfo could be allocated",
              (unsigned)mw, (unsigned)mh);
        return FALSE;
    }

    IExec->AddTail((struct List *)&bi->ResolutionsList, (struct Node *)res);
    DCHIP("chip_add_mode: %s (%ux%u) added [DisplayID=0x%08lx, %lu depths]",
          res->Name, (unsigned)mw, (unsigned)mh, res->DisplayID,
          modeinfos_created);
    return TRUE;
}

/* -----------------------------------------------------------------------
 * chip_register_modes -- populate ResolutionsList with SVGA modes.
 *
 * P96/PCIGraphics.card normally reads mode tables from the chip's ELF rodata.
 * Our chip doesn't have those tables, so we must create Resolution + ModeInfo
 * entries directly and insert them into the ResolutionsList.
 *
 * Resolution struct = 80 bytes (reverse-engineered from live P96 data).
 * ModeInfo struct = standard P96 layout (defined in boardinfo.h).
 *
 * Called from chip_InitCard_C after BoardInfo is filled.
 * ----------------------------------------------------------------------- */
void chip_register_modes(struct ChipGPUState *gs)
{
    struct ExecIFace *IExec = gs->IExec;
    struct BoardInfo *bi = gs->bi;
    if (!bi) return;

    /* Get board number string for P96ID -- P96 uses "RTG-N:" where N = BoardNum */
    UWORD board_num = bi->BoardNum;
    (void)board_num; /* used inside chip_add_mode */

    DCHIP("chip_register_modes: bi=%p BoardNum=%u", bi, (unsigned)board_num);

    /* Pass 1: register the static mode table (skip the 0/1 dummies). */
    for (ULONG m = 2; m < chip_num_modes; m++) {
        chip_add_mode(gs,
            chip_modes[m].w, chip_modes[m].h,
            chip_modes[m].hor_total, chip_modes[m].hor_sync_start, chip_modes[m].hor_sync_size,
            chip_modes[m].ver_total, chip_modes[m].ver_sync_start, chip_modes[m].ver_sync_size,
            chip_modes[m].clock, chip_modes[m].flags, m);
    }

    /* Pass 2: register every distinct EDID DTD that wasn't already in
     * the static table.  chip_add_mode itself deduplicates against the
     * live ResolutionsList, so this is a clean union; CVT-RB timings
     * synthesised inside the helper.  slot_idx is biased above the
     * static range to keep DisplayIDs collision-free. */
    if (gs->has_edid && gs->edid_dtd_count > 0) {
        for (uint32 i = 0; i < gs->edid_dtd_count; i++) {
            UWORD ew = gs->edid_dtd_w[i];
            UWORD eh = gs->edid_dtd_h[i];
            chip_add_mode(gs, ew, eh,
                          0, 0, 0, 0, 0, 0, 0, 0,
                          chip_num_modes + i);
        }
    }

    /* Dump final ResolutionsList count -- uses GetHead/GetSucc for the
     * walk per AOS4 SDK convention, with a fast path via IsListEmpty
     * (we expect at least the static modes to have registered, but
     * guarding is cheap). */
    {
        int count = 0;
        /* IsMinListEmpty is a macro from <exec/lists.h>, not an
         * IExec method -- it expands to a tailpred comparison.
         * GetHead/GetSucc are real IExec methods (per Exec_Lists_and_Queues
         * SDK guidance). */
        if (!IsMinListEmpty(&bi->ResolutionsList)) {
            struct Node *node;
            for (node = IExec->GetHead((struct List *)&bi->ResolutionsList);
                 node != NULL;
                 node = IExec->GetSucc(node)) {
                count++;
            }
        }
        DCHIP("chip_register_modes: ResolutionsList now has %d entries", count);
    }
}

/* -----------------------------------------------------------------------
 * chip_dump_displayinfobase -- diagnostic dump of every ModeID currently
 * registered in graphics.library DisplayInfoBase.  RTG modes from this
 * chip use MONITOR_ID 0x0005 (high 16 bits of DisplayID); standard Amiga
 * monitors use PAL=0x0002, NTSC=0x0001, VGA=0x0031, MULTISCAN=0x000A.
 * Anything we don't own came from DEVS:Monitors/* files loaded at startup.
 *
 * Must be called *after* the system is up (e.g. first SetSwitch(TRUE));
 * during chip_InitCard_C the database isn't fully populated yet.
 * ----------------------------------------------------------------------- */
void chip_dump_displayinfobase(struct ChipGPUState *gs)
{
    struct ExecIFace *IExec = gs->IExec;
    struct Library *gfxBase =
        IExec->OpenLibrary("graphics.library", 53);
    struct GraphicsIFace *IGraphics = gfxBase
        ? (struct GraphicsIFace *)IExec->GetInterface(gfxBase, "main", 1, NULL)
        : NULL;
    if (!IGraphics) {
        DCHIP("DisplayInfoBase enumeration: graphics.library v53 unavailable");
        if (gfxBase) IExec->CloseLibrary(gfxBase);
        return;
    }

    ULONG mid = INVALID_ID;
    int   logged = 0, scanned = 0;
    DCHIP("DisplayInfoBase enumeration (MONITOR_ID 0x0005=ours):");
    while ((mid = IGraphics->NextDisplayInfo(mid)) != INVALID_ID) {
        scanned++;
        DisplayInfoHandle handle = IGraphics->FindDisplayInfo(mid);
        if (!handle) continue;

        struct DimensionInfo dim;
        struct NameInfo ni;
        struct DisplayInfo disp;
        chip_zero(&disp, sizeof(disp));
        chip_zero(&dim,  sizeof(dim));
        chip_zero(&ni,   sizeof(ni));

        BOOL got_name = (IGraphics->GetDisplayInfoData(handle,
                (APTR)&ni, sizeof(ni), DTAG_NAME, mid) >= (LONG)sizeof(ni));
        IGraphics->GetDisplayInfoData(handle, (APTR)&disp,
                sizeof(disp), DTAG_DISP, mid);
        if (IGraphics->GetDisplayInfoData(handle, (APTR)&dim,
                sizeof(dim), DTAG_DIMS, mid) >= 48)
        {
            UWORD w = dim.Nominal.MaxX - dim.Nominal.MinX + 1;
            UWORD h = dim.Nominal.MaxY - dim.Nominal.MinY + 1;
            const char *origin = (disp.PropertyFlags & DIPF_IS_RTG) ? "RTG" : "native";
            DCHIP("  ModeID=0x%08lx mon=0x%04lx %4ux%4u d=%u flags=0x%08lx "
                  "navail=0x%04x %s name=%s",
                  mid, (mid >> 16) & 0xFFFF,
                  (unsigned)w, (unsigned)h,
                  (unsigned)dim.MaxDepth,
                  (ULONG)disp.PropertyFlags,
                  (unsigned)disp.NotAvailable,
                  origin,
                  got_name ? ni.Name : "(?)");
            if (++logged >= 80) {
                DCHIP("  ... (truncated at 80)");
                break;
            }
        }
    }
    DCHIP("DisplayInfoBase: scanned=%d logged=%d", scanned, logged);

    /* --- Hide native (non-RTG) modes from ScreenMode prefs ---
     * On Pegasos2 there is no Amiga custom chipset; graphics.library
     * still publishes a default + PAL mode table from its built-in
     * legacy compatibility list (DEVS:Monitors is empty in the user's
     * install).  ScreenMode lists every mode whose NotAvailable is
     * zero, which is why PAL ModeIDs like 0x21000 keep appearing.
     * Set NotAvailable=DI_AVAIL_NOCHIPS on every non-DIPF_IS_RTG entry
     * so ScreenMode hides them while leaving the descriptors in place
     * (legacy software that explicitly opens those ModeIDs still gets
     * back the existing data; only the prefs UI is affected). */
    {
        ULONG mid2 = INVALID_ID;
        int   hidden = 0;
        while ((mid2 = IGraphics->NextDisplayInfo(mid2)) != INVALID_ID) {
            DisplayInfoHandle h = IGraphics->FindDisplayInfo(mid2);
            if (!h) continue;
            struct DisplayInfo d;
            chip_zero(&d, sizeof(d));
            LONG got = IGraphics->GetDisplayInfoData(h, (APTR)&d,
                            sizeof(d), DTAG_DISP, mid2);
            if (got < 48) continue;
            if (d.PropertyFlags & DIPF_IS_RTG) continue;
            if (d.NotAvailable == DI_AVAIL_NOCHIPS) continue;
            d.NotAvailable = DI_AVAIL_NOCHIPS;
            IGraphics->SetDisplayInfoData(h, (APTR)&d,
                            sizeof(d), DTAG_DISP, mid2);
            hidden++;
        }
        DCHIP("DisplayInfoBase: hid %d native modes (NotAvailable=DI_AVAIL_NOCHIPS)",
              hidden);
    }
    IExec->DropInterface((struct Interface *)IGraphics);
    IExec->CloseLibrary(gfxBase);
}
