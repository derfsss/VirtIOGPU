/*
 * chip_init.c -- VirtIO GPU PCI/VirtIO init and chip_InitCard_C.
 *
 * Contains all hardware initialization: PCI BAR scanning, IMMU setup,
 * VirtIO PCI capability parsing, VirtIO 1.0 modern init sequence,
 * DMA allocation, GPU resource setup, and the main chip_InitCard_C
 * entry point that ties everything together.
 */

#include "chip/chip_state.h"

/* -----------------------------------------------------------------------
 * chip_dma_alloc -- allocate a page-aligned MEMF_SHARED buffer and
 * DMA-map it.  Returns virtual address; sets *phys_out.  Returns NULL
 * on failure.
 *
 * AmigaOS DMA protocol requires a matching EndDMA for every StartDMA.
 * Buffers allocated here are retained for the driver's lifetime (the
 * chip has no Expunge path -- it's loaded from ROM and stays resident).
 * The StartDMA mapping is therefore intentionally held open.  On init
 * failure, callers MUST invoke chip_dma_free() to unwind properly.
 * ----------------------------------------------------------------------- */
static APTR chip_dma_alloc(struct ExecIFace *IExec, uint32 size, uint32 *phys_out)
{
    APTR mem = IExec->AllocVecTags(size,
        AVT_Type,           MEMF_SHARED,
        AVT_Alignment,      4096,
        AVT_Contiguous,     TRUE,
        AVT_ClearWithValue, 0,
        TAG_END);
    if (!mem) {
        DCHIP("chip_dma_alloc: AllocVecTags failed (size=%lu MEMF_SHARED+contig+4K)",
              (unsigned long)size);
        return NULL;
    }

    uint32 n = IExec->StartDMA(mem, size, DMA_ReadFromRAM);
    if (n == 0) {
        DCHIP("chip_dma_alloc: StartDMA failed (mem=%p size=%lu)",
              mem, (unsigned long)size);
        IExec->FreeVec(mem);
        return NULL;
    }

    if (n > 1) {
        DCHIP("WARNING: DMA buffer %p (size=%lu) spans %lu physical entries -- "
              "expected contiguous", mem, size, n);
    }

    struct DMAEntry *dmaList = (struct DMAEntry *)IExec->AllocSysObjectTags(
        ASOT_DMAENTRY, ASODMAE_NumEntries, n, TAG_DONE);
    if (!dmaList) {
        DCHIP("chip_dma_alloc: DMA list alloc failed (entries=%lu size=%lu)",
              (unsigned long)n, (unsigned long)size);
        IExec->EndDMA(mem, size, DMA_ReadFromRAM | DMAF_NoModify);
        IExec->FreeVec(mem);
        return NULL;
    }

    IExec->GetDMAList(mem, size, DMA_ReadFromRAM, dmaList);
    *phys_out = (uint32)dmaList[0].PhysicalAddress;
    IExec->FreeSysObject(ASOT_DMAENTRY, dmaList);

    /* Leave the buffer at the default MEMF_SHARED attributes (implicit
     * cache-inhibited).  Matches the VirtualSCSIDevice / AmigaNVMeDevice
     * pattern: neither driver applies per-DMA-buffer MMU attributes.
     * Keeping CI here means the eieio barriers in virtqueue.c are the
     * correct I/O ordering primitive -- coherent+cached would have
     * required full sync/mbar which the shared virtqueue code does not
     * emit. */

    return mem;
}

/* -----------------------------------------------------------------------
 * chip_dma_free -- reverse of chip_dma_alloc.  Balances StartDMA with
 * EndDMA, then frees the buffer.  NULL-safe: no-op on NULL mem.
 * ----------------------------------------------------------------------- */
static void chip_dma_free(struct ExecIFace *IExec, APTR mem, uint32 size)
{
    if (!mem) return;
    IExec->EndDMA(mem, size, DMA_ReadFromRAM | DMAF_NoModify);
    IExec->FreeVec(mem);
}

/* -----------------------------------------------------------------------
 * chip_immu_update -- common IMMU attribute helper.
 *
 * Obtains the MMU interface, performs an atomic read-modify-write of the
 * attribute bits for [addr, addr+size), then releases the interface.
 * The Forbid/Permit pair protects the non-atomic Get/Set sequence from
 * preemption by other tasks that might also be editing page attributes
 * (exec itself modifies these during paging).
 *
 * `clear_mask` is removed first, then `set_mask` is OR'd in.  This lets
 * callers both add and remove attribute bits in one call -- essential for
 * PPC where W/I and W/G are mutually exclusive.
 *
 * Returns TRUE on success, FALSE if the MMU interface is unavailable
 * (which would typically indicate a broken system -- all OS4 kernels ship
 * with it).  Structured so the DropInterface call is unconditional on
 * the success path and entirely absent on failure -- no fragile early-
 * return + unconditional drop pattern.
 * ----------------------------------------------------------------------- */
static BOOL chip_immu_update(struct ExecIFace *IExec, APTR addr, uint32 size,
                              uint32 clear_mask, uint32 set_mask,
                              uint32 *out_existing, uint32 *out_new)
{
    struct ExecBase *SysBase = *(struct ExecBase **)4;
    struct MMUIFace *IMMU = (struct MMUIFace *)
        IExec->GetInterface((struct Library *)SysBase, "mmu", 1, NULL);
    if (!IMMU) {
        /* Log once per session -- if the kernel has no MMU interface,
         * every IMMU-dependent setup step (BAR + framebuffer cache
         * attributes) will silently no-op.  All AOS4 kernels ship with
         * it, so this is a strong "the system is wrong" signal. */
        static volatile UBYTE warned = 0;
        if (!warned) {
            warned = 1;
            DCHIP("chip_immu_update: MMUIFace unavailable -- BAR/FB cache "
                  "attributes NOT applied (system without MMU interface?)");
        }
        return FALSE;
    }

    /* Forbid/Permit (NOT a mutex) is the right tool here: we are
     * keeping the read-modify-write of an MMU page-attribute group
     * atomic with respect to *any* other task that might also touch
     * the same region's attributes.  A mutex would only protect
     * against contenders that took the same mutex -- but no other
     * code in the system uses our mutex for MMU updates, so there
     * would be no protection at all.  Forbid/Permit prevents task
     * switching for the brief get/set window, which is what we
     * actually need.  Held for << 1 us so system-wide impact is nil. */
    IExec->Forbid();
    uint32 existing = IMMU->GetMemoryAttrs(addr, 0);
    uint32 newattrs = (existing & ~clear_mask) | set_mask;
    IMMU->SetMemoryAttrs(addr, size, newattrs);
    IExec->Permit();

    IExec->DropInterface((struct Interface *)IMMU);

    if (out_existing) *out_existing = existing;
    if (out_new)      *out_new      = newattrs;
    return TRUE;
}

/* -----------------------------------------------------------------------
 * chip_immu_set_coherent -- set MEMATTRF_COHERENT (clear CACHEINHIBIT).
 *
 * Used on DMA buffers shared between CPU and GPU.  Keeps L1/L2 caching
 * active while enforcing hardware coherency -- both CPU and DMA see
 * consistent data without explicit cache flushing.  Much faster than
 * CACHEINHIBIT for small DMA buffers.
 *
 * CACHEINHIBIT must be cleared: CI + M together silently drops the M
 * flag (kas1e's discovery in pa6t_eth.device), leading to CPU lockups
 * when DMA writes nearby data.
 * Source: https://www.amigans.net/modules/newbb/viewtopic.php?post_id=159798
 * ----------------------------------------------------------------------- */
void chip_immu_set_coherent(struct ExecIFace *IExec, APTR addr, uint32 size)
{
    chip_immu_update(IExec, addr, size,
                     MEMATTRF_CACHEINHIBIT, MEMATTRF_COHERENT,
                     NULL, NULL);
}

/* -----------------------------------------------------------------------
 * chip_immu_set_writethrough -- set MEMATTRF_WRITETHROUGH (clear CACHEINHIBIT).
 *
 * Used on the framebuffer.  Write-through lets L2 cache coalesce sequential
 * pixel stores into 64-byte blocks while guaranteeing each write is visible
 * to DMA reads without explicit cache flushing.  W and I are mutually
 * exclusive per the PPC architecture, so CI must be cleared first.
 * ----------------------------------------------------------------------- */
void chip_immu_set_writethrough(struct ExecIFace *IExec, APTR addr, uint32 size)
{
    chip_immu_update(IExec, addr, size,
                     MEMATTRF_CACHEINHIBIT, MEMATTRF_WRITETHROUGH,
                     NULL, NULL);
}

/* -----------------------------------------------------------------------
 * chip_immu_setup_bar -- set CACHEINHIBIT + GUARDED on a PCI BAR region.
 *
 * Essential for correct MMIO behaviour.  Without these bits the PPC can
 * speculatively reorder MMIO loads/stores and gather them in cache,
 * corrupting the GPU's register state.  Pattern from RadeonRX.chip.
 * ----------------------------------------------------------------------- */
static void chip_immu_setup_bar(struct ExecIFace *IExec, struct PCIResourceRange *bar)
{
    if (!bar || !(bar->Flags & PCI_RANGE_MEMORY))
        return;

    uint32 existing = 0, newattrs = 0;
    if (!chip_immu_update(IExec, (APTR)bar->BaseAddress, bar->Size,
                           0,  /* don't clear anything -- preserve platform bits */
                           MEMATTRF_CACHEINHIBIT | MEMATTRF_GUARDED,
                           &existing, &newattrs)) {
        DCHIP("WARNING: Could not get MMU interface -- MMIO may be unreliable");
        return;
    }
    DCHIP("IMMU: BAR @ 0x%08lx size=0x%lx attrs 0x%lx -> 0x%lx (CI+G)",
          bar->BaseAddress, bar->Size, existing, newattrs);
}

/* -----------------------------------------------------------------------
 * chip_resize_fb_to_mode -- recreate the framebuffer resource at the new
 * active dimensions.  Called from chip_SetGC on screen-mode change.
 *
 * QEMU's gl=on display backend sizes the SDL window to the *resource*
 * dimensions (egl_fb_blit ignores the scanout sub-rect), so the resource
 * MUST equal the active scanout size for the host window and the
 * AmigaOS rendering to align.  Mode change therefore destroys the
 * current framebuffer/resource pair and rebuilds at the new size.
 *
 * Returns TRUE on success.  On failure leaves the old resource intact
 * so the caller can keep displaying.
 * ----------------------------------------------------------------------- */
BOOL chip_resize_fb_to_mode(struct ChipGPUState *gs, uint32 new_w, uint32 new_h)
{
    struct ExecIFace *IExec = gs->IExec;
    if (new_w == 0 || new_h == 0) return FALSE;
    if (new_w == gs->fb_width && new_h == gs->fb_height) return TRUE;

    DCHIP("resize_fb: %lux%lu -> %lux%lu",
          (ULONG)gs->fb_width, (ULONG)gs->fb_height, (ULONG)new_w, (ULONG)new_h);

    uint32 new_stride = new_w * 4;
    uint32 new_size   = new_h * new_stride;

    /* 1. Allocate new fb_mem (page-aligned, MEMF_SHARED, zeroed) */
    APTR new_mem = IExec->AllocVecTags(new_size,
        AVT_Type,           MEMF_SHARED,
        AVT_Alignment,      4096,
        AVT_ClearWithValue, 0,
        TAG_END);
    if (!new_mem) {
        DCHIP("resize_fb: AllocVecTags(%lu) failed", (ULONG)new_size);
        return FALSE;
    }

    /* 2. StartDMA + GetDMAList for new fb */
    uint32 new_dma_count = IExec->StartDMA(new_mem, new_size, DMA_ReadFromRAM);
    if (new_dma_count == 0) {
        DCHIP("resize_fb: StartDMA failed");
        IExec->FreeVec(new_mem);
        return FALSE;
    }
    struct DMAEntry *new_dma_list = (struct DMAEntry *)IExec->AllocSysObjectTags(
        ASOT_DMAENTRY, ASODMAE_NumEntries, new_dma_count, TAG_DONE);
    if (!new_dma_list) {
        DCHIP("resize_fb: DMA list alloc failed");
        IExec->EndDMA(new_mem, new_size, DMA_ReadFromRAM | DMAF_NoModify);
        IExec->FreeVec(new_mem);
        return FALSE;
    }
    IExec->GetDMAList(new_mem, new_size, DMA_ReadFromRAM, new_dma_list);
    uint32 new_phys = (uint32)new_dma_list[0].PhysicalAddress;
    chip_immu_set_writethrough(IExec, new_mem, new_size);

    /* 3. Allocate a fresh resource ID and CREATE_2D + ATTACH_BACKING.
     *    Use a high-numbered ID that doesn't collide with cursor (2),
     *    legacy fb (1), or anything chip_alloc_resource_id might mint. */
    uint32 new_res_id = chip_alloc_resource_id(gs);
    if (!chip_ResourceCreate2D(gs, new_res_id,
                               VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM,
                               new_w, new_h)) {
        DCHIP("resize_fb: RESOURCE_CREATE_2D %lu (%lux%lu) failed",
              (ULONG)new_res_id, (ULONG)new_w, (ULONG)new_h);
        goto fail_dma;
    }
    if (!chip_ResourceAttachBacking(gs, new_res_id, new_dma_list, new_dma_count)) {
        DCHIP("resize_fb: ATTACH_BACKING failed");
        chip_ResourceUnref(gs, new_res_id);
        goto fail_dma;
    }

    /* 4. Switch scanout to the new resource.  This triggers
     *    qemu_console_resize -> sdl2_gl_switch -> SDL_SetWindowSize. */
    if (!chip_SetScanout(gs, 0, new_res_id, 0, 0, new_w, new_h)) {
        DCHIP("resize_fb: SET_SCANOUT failed");
        chip_ResourceUnref(gs, new_res_id);
        goto fail_dma;
    }

    /* 5. Tear down the old resource + DMA mapping.  Do this AFTER the
     *    new scanout is live so a flush task running concurrently never
     *    sees a stale resource_id. */
    uint32 old_res = gs->fb_resource_id;
    APTR   old_mem = gs->fb_mem;
    uint32 old_size = gs->fb_size;
    struct DMAEntry *old_dma_list = gs->fb_dma_list;

    gs->fb_mem        = new_mem;
    gs->fb_phys       = new_phys;
    gs->fb_width      = new_w;
    gs->fb_height     = new_h;
    gs->fb_stride     = new_stride;
    gs->fb_size       = new_size;
    gs->fb_dma_count  = new_dma_count;
    gs->fb_dma_list   = new_dma_list;
    gs->resource_id   = new_res_id;
    gs->fb_resource_id = new_res_id;

    if (old_res) chip_ResourceUnref(gs, old_res);
    if (old_dma_list) IExec->FreeSysObject(ASOT_DMAENTRY, old_dma_list);
    if (old_mem) {
        IExec->EndDMA(old_mem, old_size, DMA_ReadFromRAM | DMAF_NoModify);
        IExec->FreeVec(old_mem);
    }

    DCHIP("resize_fb: OK new_res=%lu (%lux%lu, %lu bytes)",
          (ULONG)new_res_id, (ULONG)new_w, (ULONG)new_h, (ULONG)new_size);
    return TRUE;

fail_dma:
    IExec->FreeSysObject(ASOT_DMAENTRY, new_dma_list);
    IExec->EndDMA(new_mem, new_size, DMA_ReadFromRAM | DMAF_NoModify);
    IExec->FreeVec(new_mem);
    return FALSE;
}

/* -----------------------------------------------------------------------
 * chip_scan_bars -- enumerate all 6 PCI BARs via GetResourceRange.
 * Logs each BAR's type/address/size, sets IMMU CI+G on memory BARs,
 * stores info in gs->bars[], and calls FreeResourceRange after extraction.
 * Must be called AFTER pciDev is locked.
 * ----------------------------------------------------------------------- */
static void chip_scan_bars(struct ChipGPUState *gs)
{
    struct ExecIFace *IExec = gs->IExec;
    struct PCIDevice *pciDev = gs->pciDevice;

    for (int i = 0; i < 6; i++) {
        gs->bars[i].valid = FALSE;
        gs->bars[i].base  = 0;
        gs->bars[i].size  = 0;

        struct PCIResourceRange *range = pciDev->GetResourceRange(i);
        if (!range) {
            DCHIP("BAR%d: not present", i);
            continue;
        }

        gs->bars[i].base     = range->BaseAddress;
        gs->bars[i].size     = range->Size;
        gs->bars[i].physical = range->Physical;
        gs->bars[i].flags    = range->Flags;
        gs->bars[i].valid    = TRUE;

        const char *type = "UNKNOWN";
        if (range->Flags & PCI_RANGE_IO)
            type = "I/O";
        else if (range->Flags & PCI_RANGE_MEMORY)
            type = (range->Flags & PCI_RANGE_PREFETCH) ? "MEM+PREFETCH" : "MEM";

        DCHIP("BAR%d: %s base=0x%08lx phys=0x%08lx size=0x%lx",
              i, type, range->BaseAddress, range->Physical, range->Size);

        /* Set IMMU cache-inhibit + guarded on memory BARs */
        if (range->Flags & PCI_RANGE_MEMORY) {
            chip_immu_setup_bar(IExec, range);
        }

        pciDev->FreeResourceRange(range);
    }
}

/* -----------------------------------------------------------------------
 * chip_scan_pci_caps -- scan VirtIO PCI capabilities.
 * Mirrors pci_modern_detect.c but operates on ChipGPUState.
 * Must be called AFTER chip_scan_bars() populates gs->bars[].
 * ----------------------------------------------------------------------- */
static BOOL chip_scan_pci_caps(struct ChipGPUState *gs)
{
    struct PCIDevice *pciDev = gs->pciDevice;

    gs->modern_mode      = FALSE;
    gs->common_cfg_base  = 0;
    gs->notify_cfg_base  = 0;
    gs->notify_off_mult  = 0;
    gs->isr_cfg_base     = 0;
    gs->device_cfg_base  = 0;
    gs->use_pci_cfg      = FALSE;
    gs->pci_cfg_cap_off  = 0;

    struct PCICapability *cap = pciDev->GetFirstCapability();
    int guard = 0;

    while (cap && guard < 32) {
        guard++;

        if (cap->Type != PCI_CAPABILITYID_VENDOR) {
            cap = pciDev->GetNextCapability(cap);
            continue;
        }

        uint8  cfg_type = pciDev->ReadConfigByte(cap->CapOffset + VIRTIO_CAP_OFF_CFG_TYPE);
        uint8  bar_num  = pciDev->ReadConfigByte(cap->CapOffset + VIRTIO_CAP_OFF_BAR);
        uint32 offset   = pciDev->ReadConfigLong(cap->CapOffset + VIRTIO_CAP_OFF_OFFSET);

        /* PCI_CFG cap (type 5) -- save offset for AmigaOne fallback */
        if (cfg_type == VIRTIO_PCI_CAP_PCI_CFG) {
            gs->pci_cfg_cap_off = (uint8)cap->CapOffset;
            DCHIP("PCI_CFG cap at config offset 0x%02x", (uint32)gs->pci_cfg_cap_off);
            cap = pciDev->GetNextCapability(cap);
            continue;
        }

        /* Store BAR-relative offsets (needed for PCI_CFG fallback path) */
        switch (cfg_type) {
        case VIRTIO_PCI_CAP_COMMON_CFG:
            gs->common_cfg_bar = bar_num;
            gs->common_cfg_off = offset;
            gs->modern_mode = TRUE;
            break;
        case VIRTIO_PCI_CAP_NOTIFY_CFG:
            gs->notify_cfg_bar = bar_num;
            gs->notify_cfg_off = offset;
            gs->notify_off_mult = pciDev->ReadConfigLong(
                cap->CapOffset + VIRTIO_CAP_OFF_NOTIFY_MULT);
            break;
        case VIRTIO_PCI_CAP_ISR_CFG:
            gs->isr_cfg_bar = bar_num;
            gs->isr_cfg_off = offset;
            break;
        case VIRTIO_PCI_CAP_DEVICE_CFG:
            gs->device_cfg_bar = bar_num;
            gs->device_cfg_off = offset;
            break;
        default:
            break;
        }

        /* Compute virtual address for MMIO path (may fail on AmigaOne) */
        if (bar_num < 6 && gs->bars[bar_num].valid &&
            (gs->bars[bar_num].flags & PCI_RANGE_MEMORY))
        {
            uint32 virt_addr = gs->bars[bar_num].base + offset;
            switch (cfg_type) {
            case VIRTIO_PCI_CAP_COMMON_CFG:
                gs->common_cfg_base = virt_addr;
                DCHIP("COMMON_CFG BAR%u+0x%lx -> 0x%08lx", (uint32)bar_num, offset, virt_addr);
                break;
            case VIRTIO_PCI_CAP_NOTIFY_CFG:
                gs->notify_cfg_base = virt_addr;
                DCHIP("NOTIFY_CFG BAR%u+0x%lx -> 0x%08lx mult=%lu",
                      (uint32)bar_num, offset, virt_addr, gs->notify_off_mult);
                break;
            case VIRTIO_PCI_CAP_ISR_CFG:
                gs->isr_cfg_base = virt_addr;
                DCHIP("ISR_CFG BAR%u+0x%lx -> 0x%08lx", (uint32)bar_num, offset, virt_addr);
                break;
            case VIRTIO_PCI_CAP_DEVICE_CFG:
                gs->device_cfg_base = virt_addr;
                DCHIP("DEVICE_CFG BAR%u+0x%lx -> 0x%08lx", (uint32)bar_num, offset, virt_addr);
                break;
            default:
                DCHIP("VirtIO PCI cap type %u BAR%u offset 0x%lx -- "
                      "unhandled cfg_type, ignored",
                      (uint32)cfg_type, (uint32)bar_num, offset);
                break;
            }
        } else {
            DCHIP("Cap type %u BAR%u: no MMIO (PCI_CFG fallback available)",
                  (uint32)cfg_type, (uint32)bar_num);
        }

        cap = pciDev->GetNextCapability(cap);
    }

    DCHIP("PCI cap scan: common=0x%08lx notify=0x%08lx isr=0x%08lx dev=0x%08lx pci_cfg=0x%02x",
          gs->common_cfg_base, gs->notify_cfg_base,
          gs->isr_cfg_base, gs->device_cfg_base, (uint32)gs->pci_cfg_cap_off);

    if (!gs->modern_mode) {
        DCHIP("ERROR: No COMMON_CFG found -- cannot access VirtIO registers");
        return FALSE;
    }

    /* Probe MMIO: write ACK to STATUS, read back.  If it returns 0x00,
     * direct MMIO doesn't work (AmigaOne / Articia S bridge).
     *
     * PCI_CFG fallback (VirtIO spec 4.1.4.7) is implemented in VIO_* macros
     * via gs->use_pci_cfg dispatch, but intentionally NOT auto-enabled here:
     * QEMU's -M amigaone machine model has a bug in hw/ppc/amigaone.c where
     * the Articia S bridge does not register PCI BAR memory regions.  Any
     * PCI_CFG access hits an assertion failure inside virtio_address_space_lookup
     * and crashes QEMU.  Until QEMU is fixed, fail cleanly on non-Pegasos2.
     *
     * On real hardware or a fixed QEMU, enabling use_pci_cfg manually would
     * route all VIO_* accesses through PCI config space (ReadConfigLong /
     * WriteConfigLong) which the Articia S does forward correctly. */
    if (!gs->common_cfg_base) {
        DCHIP("ERROR: No MMIO common_cfg_base -- unsupported platform");
        DCHIP("  This driver requires QEMU -M pegasos2 (MV64361 bridge).");
        return FALSE;
    }

    mmio_write8(gs->common_cfg_base + VIRTIO_PCI_COMMON_STATUS, 0x00);  /* reset */
    mmio_write8(gs->common_cfg_base + VIRTIO_PCI_COMMON_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    uint8 probe = mmio_read8(gs->common_cfg_base + VIRTIO_PCI_COMMON_STATUS);
    mmio_write8(gs->common_cfg_base + VIRTIO_PCI_COMMON_STATUS, 0x00);  /* reset */

    if (probe != VIRTIO_STATUS_ACKNOWLEDGE) {
        DCHIP("ERROR: MMIO probe FAILED (status=0x%02x, expect 0x01)", (uint32)probe);
        DCHIP("  Direct MMIO to PCI BARs does not work on this machine.");
        DCHIP("  This driver requires QEMU -M pegasos2 (MV64361 bridge).");
        DCHIP("  AmigaOne (Articia S) is NOT SUPPORTED -- QEMU bug in PCI_CFG path.");
        return FALSE;
    }

    DCHIP("MMIO probe OK (status=0x%02x) -- using direct MMIO", (uint32)probe);
    gs->use_pci_cfg = FALSE;
    return TRUE;
}

/* -----------------------------------------------------------------------
 * chip_virtio_init -- VirtIO 1.0 modern init sequence.
 * Mirrors InitVirtIO_Modern from virtio_init.c.
 * Sets up control queue (0) only -- no cursor queue needed for basic FB.
 * ----------------------------------------------------------------------- */
static BOOL chip_virtio_init(struct ChipGPUState *gs)
{
    struct ExecIFace *IExec  = gs->IExec;

    /* PCI_COMMAND_MEMORY + MASTER + INT_DISABLE already set in chip_InitCard_C
     * before chip_scan_bars/chip_scan_pci_caps (required for PCI_CFG fallback). */

    /* Step 1: Reset */
    VIO_W8(gs, VIRTIO_PCI_COMMON_STATUS, 0x00);
    {
        uint32 tries = 0;
        uint8  rst;
        do {
            rst = VIO_R8(gs, VIRTIO_PCI_COMMON_STATUS);
        } while (rst != 0 && ++tries < 1000);
        DCHIP("After reset: status=0x%02x (tries=%lu)", (uint32)VIO_R8(gs, VIRTIO_PCI_COMMON_STATUS), tries);
    }

    /* Step 2: ACKNOWLEDGE */
    VIO_W8(gs, VIRTIO_PCI_COMMON_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    {
        uint8 ack = VIO_R8(gs, VIRTIO_PCI_COMMON_STATUS);
        DCHIP("After ACK write: status=0x%02x (expect 0x01)", (uint32)ack);
    }

    /* Step 3: DRIVER */
    VIO_W8(gs, VIRTIO_PCI_COMMON_STATUS,
           VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    {
        uint8 drv = VIO_R8(gs, VIRTIO_PCI_COMMON_STATUS);
        DCHIP("After DRIVER write: status=0x%02x (expect 0x03)", (uint32)drv);
    }

    /* Step 4: Feature negotiation -- accept VERSION_1 + EDID */
    VIO_W32(gs, VIRTIO_PCI_COMMON_DFSELECT, 0);
    uint32 dev_feat_lo = VIO_R32(gs, VIRTIO_PCI_COMMON_DF);
    VIO_W32(gs, VIRTIO_PCI_COMMON_DFSELECT, 1);
    uint32 dev_feat_hi = VIO_R32(gs, VIRTIO_PCI_COMMON_DF);

    DCHIP("Device features lo=0x%08lx hi=0x%08lx", dev_feat_lo, dev_feat_hi);

    uint32 drv_feat_lo = dev_feat_lo & VIRTIO_DEVICE_FEATURES_MASK;
    uint32 drv_feat_hi = dev_feat_hi & 1UL; /* VERSION_1 */

    /* Track which features we actually negotiated */
    gs->has_virgl = (drv_feat_lo & VIRTIO_GPU_F_VIRGL) ? TRUE : FALSE;
    gs->has_blob  = (drv_feat_lo & VIRTIO_GPU_F_RESOURCE_BLOB) ? TRUE : FALSE;
    DCHIP("Feature negotiation: VIRGL=%s EDID=%s BLOB=%s",
          gs->has_virgl ? "YES" : "NO",
          (drv_feat_lo & VIRTIO_GPU_F_EDID) ? "YES" : "NO",
          gs->has_blob ? "YES" : "NO");

    /* User-visible diagnostic when Virgl is unavailable.  Without Virgl we
     * cannot provide hardware compositing (DIPF_IS_HWCOMPOSITE stays unset)
     * or future MiniGL/Warp3D acceleration -- 2D framebuffer only.  Running
     * QEMU with "-device virtio-gpu-pci,virgl=on" (and a host GL stack
     * such as mesa/libvirglrenderer) exposes the VIRGL feature bit and
     * unlocks GPU-accelerated compositing. */
    if (!gs->has_virgl) {
        DCHIP("NOTE: Virgl NOT negotiated -- 2D framebuffer mode only.");
        DCHIP("      Hardware compositing + 3D require QEMU launched with:");
        DCHIP("          -device virtio-gpu-pci,virgl=on");
        DCHIP("      plus a host GL stack (mesa + libvirglrenderer).");
    }

    VIO_W32(gs, VIRTIO_PCI_COMMON_DFSELECTG, 0);
    VIO_W32(gs, VIRTIO_PCI_COMMON_DFG, drv_feat_lo);
    VIO_W32(gs, VIRTIO_PCI_COMMON_DFSELECTG, 1);
    VIO_W32(gs, VIRTIO_PCI_COMMON_DFG, drv_feat_hi);

    /* Step 5: FEATURES_OK */
    VIO_W8(gs, VIRTIO_PCI_COMMON_STATUS,
           VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);

    /* Step 6: verify FEATURES_OK */
    uint8 status_check = VIO_R8(gs, VIRTIO_PCI_COMMON_STATUS);
    if (!(status_check & VIRTIO_STATUS_FEATURES_OK)) {
        DCHIP("FEATURES_OK rejected! status=0x%02x", (uint32)status_check);
        VIO_W8(gs, VIRTIO_PCI_COMMON_STATUS, VIRTIO_STATUS_FAILED);
        return FALSE;
    }

    /* Step 7: Setup control queue (0) only */
    VIO_W16(gs, VIRTIO_PCI_COMMON_Q_SELECT, 0);
    uint16 q_max = VIO_R16(gs, VIRTIO_PCI_COMMON_Q_SIZE);
    DCHIP("Queue 0 max size: %u", (uint32)q_max);
    if (q_max == 0) {
        DCHIP("Queue 0 unavailable!");
        return FALSE;
    }

    struct virtqueue *vq = VirtQueue_Allocate(IExec, 0, q_max);
    if (!vq) {
        DCHIP("VirtQueue_Allocate failed");
        return FALSE;
    }

    uint32 vring_entries = IExec->StartDMA(vq->desc, vq->mem_size, DMA_ReadFromRAM);
    if (vring_entries == 0) {
        DCHIP("StartDMA for vring failed");
        VirtQueue_Free(IExec, vq);
        return FALSE;
    }

    if (vring_entries > 1) {
        DCHIP("WARNING: vring Q0 spans %lu DMA entries -- physical addresses "
              "may be non-contiguous", vring_entries);
    }

    struct DMAEntry *vring_dma = (struct DMAEntry *)IExec->AllocSysObjectTags(
        ASOT_DMAENTRY, ASODMAE_NumEntries, vring_entries, TAG_DONE);
    if (!vring_dma) {
        DCHIP("vring DMA list alloc failed (entries=%lu)", vring_entries);
        IExec->EndDMA(vq->desc, vq->mem_size, DMA_ReadFromRAM | DMAF_NoModify);
        VirtQueue_Free(IExec, vq);
        return FALSE;
    }

    IExec->GetDMAList(vq->desc, vq->mem_size, DMA_ReadFromRAM, vring_dma);
    uint32 desc_phys = (uint32)vring_dma[0].PhysicalAddress;
    IExec->FreeSysObject(ASOT_DMAENTRY, vring_dma);

    vq->dma_phys    = desc_phys;
    vq->dma_entries = vring_entries;

    /* Leave vring memory at default MEMF_SHARED attributes (implicit CI).
     * Matches VirtualSCSIDevice / AmigaNVMeDevice which also leave their
     * queue memory CI.  The shared virtqueue.c uses eieio as the ordering
     * primitive, which is correct for cache-inhibited I/O memory.  Setting
     * MEMATTRF_COHERENT here would require sync/mbar barriers instead of
     * eieio -- the earlier coherent setup silently mismatched the shared
     * barrier code. */

    /* Calculate avail/used physical addresses */
    uint32 desc_size   = sizeof(struct vring_desc) * q_max;
    uint32 avail_size  = sizeof(uint16) * (3 + q_max);
    uint32 used_offset = (desc_size + avail_size + 4095U) & ~4095U;

    vq->avail_phys = desc_phys + desc_size;
    vq->used_phys  = desc_phys + used_offset;

    /* Write physical addresses to device */
    VIO_W32(gs, VIRTIO_PCI_COMMON_Q_DESCLO,  desc_phys);
    VIO_W32(gs, VIRTIO_PCI_COMMON_Q_DESCHI,  0);
    VIO_W32(gs, VIRTIO_PCI_COMMON_Q_AVAILLO, vq->avail_phys);
    VIO_W32(gs, VIRTIO_PCI_COMMON_Q_AVAILHI, 0);
    VIO_W32(gs, VIRTIO_PCI_COMMON_Q_USEDLO,  vq->used_phys);
    VIO_W32(gs, VIRTIO_PCI_COMMON_Q_USEDHI,  0);
    VIO_W16(gs, VIRTIO_PCI_COMMON_Q_ENABLE,  1);

    /* Compute notify addresses for queue 0 (both MMIO and PCI_CFG paths) */
    uint16 q_noff = VIO_R16(gs, VIRTIO_PCI_COMMON_Q_NOFF);
    vq->notify_addr = gs->notify_cfg_base +
                      (gs->notify_off_mult ? (uint32)q_noff * gs->notify_off_mult : 0);
    vq->notify_bar_off = gs->notify_cfg_off +
                      (gs->notify_off_mult ? (uint32)q_noff * gs->notify_off_mult : 0);

    vq->modern          = TRUE;
    /* EVENT_IDX not enabled: driver submits one sync command at a time
     * (TransferToHost -> SetScanout -> ResourceFlush all block on response),
     * so there's no submission pipeline to coalesce.  EVENT_IDX benefit
     * requires burst-submit pattern which doesn't apply here.  Also not
     * in VIRTIO_DEVICE_FEATURES_MASK so never negotiated. */
    vq->use_event_idx   = FALSE;
    vq->last_kick_avail_idx = 0xFFFF;
    vq->use_indirect    = FALSE;

    gs->vqs[0]    = vq;
    gs->num_queues = 1;

    DCHIP("Queue 0 ready: desc=0x%08lx avail=0x%08lx used=0x%08lx notify=0x%08lx",
          desc_phys, vq->avail_phys, vq->used_phys, vq->notify_addr);

    /* Step 7b: Setup cursor queue (1) for hardware cursor */
    VIO_W16(gs, VIRTIO_PCI_COMMON_Q_SELECT, VIRTIO_GPU_CURSQ);
    uint16 cq_max = VIO_R16(gs, VIRTIO_PCI_COMMON_Q_SIZE);
    DCHIP("Queue 1 (cursor) max size: %u", (uint32)cq_max);
    if (cq_max > 0) {
        struct virtqueue *cq = VirtQueue_Allocate(IExec, VIRTIO_GPU_CURSQ, cq_max);
        if (cq) {
            uint32 cq_dma_n = IExec->StartDMA(cq->desc, cq->mem_size, DMA_ReadFromRAM);
            if (cq_dma_n > 0) {
                if (cq_dma_n > 1) {
                    DCHIP("WARNING: vring Q1 spans %lu DMA entries -- physical "
                          "addresses may be non-contiguous", cq_dma_n);
                }
                struct DMAEntry *cq_dma = (struct DMAEntry *)IExec->AllocSysObjectTags(
                    ASOT_DMAENTRY, ASODMAE_NumEntries, cq_dma_n, TAG_DONE);
                if (cq_dma) {
                    IExec->GetDMAList(cq->desc, cq->mem_size, DMA_ReadFromRAM, cq_dma);
                    uint32 cq_desc_phys = (uint32)cq_dma[0].PhysicalAddress;
                    IExec->FreeSysObject(ASOT_DMAENTRY, cq_dma);

                    cq->dma_phys    = cq_desc_phys;
                    cq->dma_entries = cq_dma_n;

                    /* Leave cursor vring at default (implicit CI) -- see
                     * ctrlq vring comment above for rationale. */

                    uint32 cq_desc_size  = sizeof(struct vring_desc) * cq_max;
                    uint32 cq_avail_size = sizeof(uint16) * (3 + cq_max);
                    uint32 cq_used_off   = (cq_desc_size + cq_avail_size + 4095U) & ~4095U;

                    cq->avail_phys = cq_desc_phys + cq_desc_size;
                    cq->used_phys  = cq_desc_phys + cq_used_off;

                    VIO_W32(gs, VIRTIO_PCI_COMMON_Q_DESCLO,  cq_desc_phys);
                    VIO_W32(gs, VIRTIO_PCI_COMMON_Q_DESCHI,  0);
                    VIO_W32(gs, VIRTIO_PCI_COMMON_Q_AVAILLO, cq->avail_phys);
                    VIO_W32(gs, VIRTIO_PCI_COMMON_Q_AVAILHI, 0);
                    VIO_W32(gs, VIRTIO_PCI_COMMON_Q_USEDLO,  cq->used_phys);
                    VIO_W32(gs, VIRTIO_PCI_COMMON_Q_USEDHI,  0);
                    VIO_W16(gs, VIRTIO_PCI_COMMON_Q_ENABLE,  1);

                    uint16 cq_noff = VIO_R16(gs, VIRTIO_PCI_COMMON_Q_NOFF);
                    cq->notify_addr = gs->notify_cfg_base +
                        (gs->notify_off_mult ? (uint32)cq_noff * gs->notify_off_mult : 0);
                    cq->notify_bar_off = gs->notify_cfg_off +
                        (gs->notify_off_mult ? (uint32)cq_noff * gs->notify_off_mult : 0);

                    cq->modern          = TRUE;
                    cq->use_event_idx   = FALSE;
                    cq->last_kick_avail_idx = 0xFFFF;
                    cq->use_indirect    = FALSE;

                    gs->vqs[VIRTIO_GPU_CURSQ] = cq;
                    gs->num_queues = 2;

                    DCHIP("Queue 1 (cursor) ready: desc=0x%08lx notify=0x%08lx",
                          cq_desc_phys, cq->notify_addr);
                } else {
                    IExec->EndDMA(cq->desc, cq->mem_size, DMA_ReadFromRAM | DMAF_NoModify);
                    VirtQueue_Free(IExec, cq);
                    DCHIP("Queue 1 (cursor): DMA entry alloc failed, cursor disabled");
                }
            } else {
                VirtQueue_Free(IExec, cq);
                DCHIP("Queue 1 (cursor): StartDMA failed, cursor disabled");
            }
        } else {
            DCHIP("Queue 1 (cursor): VirtQueue_Allocate failed, cursor disabled");
        }
    } else {
        DCHIP("Queue 1 (cursor): unavailable, cursor disabled");
    }

    /* Step 8: DRIVER_OK */
    VIO_W8(gs, VIRTIO_PCI_COMMON_STATUS,
           VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
           VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    uint8 final = VIO_R8(gs, VIRTIO_PCI_COMMON_STATUS);
    DCHIP("VirtIO init complete. Status=0x%02x", (uint32)final);

    return TRUE;
}

/* -----------------------------------------------------------------------
 * chip_ConfigureDisplay -- EDID callback for graphics.library.
 *
 * graphics.library calls this via the bi+1714 mode list entry to obtain
 * EDID data for monitor capability discovery.  This is the same mechanism
 * used by SM502.chip -- a 128-byte entry at bi+1714 with a function pointer
 * at offset 56 that gets called with (entry, p2, p3, p4, outBuf, totalBytes).
 *
 * Calling convention: r3=entry, r7=outBuf (5th PPC arg), r8=totalBytes.
 * ----------------------------------------------------------------------- */
static LONG chip_ConfigureDisplay(APTR entry, ULONG p2, ULONG p3,
                                   ULONG p4, UBYTE *outBuf, ULONG totalBytes)
{
    (void)entry; (void)p2; (void)p3; (void)p4;
    struct ChipGPUState *gs = g_chip_state;

    DCHIP("ConfigureDisplay: outBuf=%p totalBytes=%lu", outBuf, totalBytes);

    if (!outBuf || totalBytes == 0 || !gs)
        return 0;

    if (gs->has_edid && gs->edid_size > 0) {
        uint32 copyLen = totalBytes < gs->edid_size ? totalBytes : gs->edid_size;
        for (uint32 i = 0; i < copyLen; i++)
            outBuf[i] = gs->edid[i];
        DCHIP("ConfigureDisplay: wrote %lu bytes of real EDID", copyLen);
        return 1;
    }

    DCHIP("ConfigureDisplay: no EDID available");
    return 0;
}

/* -----------------------------------------------------------------------
 * chip_register_edid -- register EDID callback at bi+1714.
 *
 * graphics.library v54+ discovers monitor capabilities through a mode list
 * at BoardInfo+1714 (MinList).  Each entry is a 128-byte struct with:
 *   offset 10: display name (const char *)
 *   offset 32: width base (ULONG)
 *   offset 36: height base (ULONG)
 *   offset 40: BoardInfo pointer
 *   offset 44: EDID buffer pointer
 *   offset 56: ConfigureDisplay callback
 *
 * A gate value at bi+1634 must be > 1777 for this mechanism to be active.
 * Pattern reverse-engineered from SM502.chip (sm502_AllocDisplayMem).
 * ----------------------------------------------------------------------- */
static void chip_register_edid(struct ChipGPUState *gs, struct BoardInfo *bi)
{
    struct ExecIFace *IExec = gs->IExec;

    /* Graphics.library sets the EDID gate when the mode-list registration
     * mechanism is available.  Only proceed if the gate exceeds the known
     * minimum -- otherwise the mode list pointer is not valid. */
    uint32 gate = boardinfo_edid_gate(bi);
    DCHIP("EDID registration: gate = %lu (min %u)",
          gate, (unsigned)BOARDINFO_EDID_GATE_MIN);
    if (gate <= BOARDINFO_EDID_GATE_MIN) {
        DCHIP("EDID registration: gate check failed, skipping");
        return;
    }

    /* Allocate 128-byte entry */
    uint8 *entry = (uint8 *)IExec->AllocVecTags(128,
        AVT_Type, MEMF_SHARED, AVT_ClearWithValue, 0, TAG_DONE);
    if (!entry) {
        DCHIP("EDID registration: alloc failed");
        return;
    }

    static const char disp_name[] = "VirtIOGPU";
    *(const char **)(entry + 10)       = disp_name;
    *(uint32 *)(entry + 32)            = 40;
    *(uint32 *)(entry + 36)            = 100;
    *(struct BoardInfo **)(entry + 40)  = bi;
    *(APTR *)(entry + 44)              = (APTR)(entry + 64);
    *(APTR *)(entry + 56)              = (APTR)chip_ConfigureDisplay;

    struct MinList *modeList = boardinfo_edid_modelist(bi);
    IExec->AddTail((struct List *)modeList, (struct Node *)entry);

    DCHIP("EDID registration: entry=%p added to EDID modelist", entry);
}

/* -----------------------------------------------------------------------
 * chip_InitCard_C -- main entry point called from chip_FindCard.
 *
 * Performs full VirtIO GPU init inline (no virtiogpu.device dependency).
 * Fills BoardInfo and returns TRUE on success.
 * ----------------------------------------------------------------------- */
BOOL chip_InitCard_C(struct BoardInfo *bi, char **toolTypes, APTR cardDesc)
{
    (void)toolTypes;

    /* Bootstrap IExec from AbsExecBase */
    struct ExecBase  *SysBase = *(struct ExecBase **)4;
    struct ExecIFace *IExec   = (struct ExecIFace *)SysBase->MainInterface;
    g_IExec_early = IExec;

    DCHIP("InitCard_C v53.65 entry: bi=%p cardDesc=%p SysBase=%p", bi, cardDesc, SysBase);
    DCHIP("GPU format=B8G8R8X8_UNORM(2) -- LE-host pixman maps to "
          "x8r8g8b8 (Cairo ARGB32) for both pixman + GL backends");

    /* --- Check if VirtIOGPUBoard already stored a PCI device --- */
    struct PCIDevice *pciDev = boardinfo_get_pcidevice(bi);
    if (pciDev) {
        DCHIP("PCI device pre-set by VirtIOGPUBoard: %p", pciDev);
    }

    /* --- Track acquired resources for goto cleanup --- */
    struct Library  *ExpBase = NULL;
    struct PCIIFace *IPCI    = NULL;
    struct ChipGPUState *gs  = NULL;
    BOOL pci_locked          = FALSE;

    /* --- Open expansion.library + PCI interface --- */
    ExpBase = IExec->OpenLibrary("expansion.library", 54);
    if (!ExpBase) {
        DCHIP("Failed to open expansion.library");
        return FALSE;
    }
    IPCI = (struct PCIIFace *)IExec->GetInterface(ExpBase, "pci", 1, NULL);
    if (!IPCI) {
        DCHIP("Failed to get IPCI interface");
        goto fail_exp;
    }

    /* --- Find VirtIO GPU PCI device 1AF4:1050 (if not already provided) --- */
    if (!pciDev) {
        pciDev = IPCI->FindDeviceTags(
            FDT_VendorID, 0x1AF4,
            FDT_DeviceID, 0x1050,
            TAG_DONE);
    }

    if (!pciDev) {
        DCHIP("VirtIO GPU (1AF4:1050) not found");
        goto fail_ipci;
    }
    DCHIP("VirtIO GPU found: %p", pciDev);

    /* --- Lock PCI device for exclusive access --- */
    if (!pciDev->Lock(PCI_LOCK_EXCLUSIVE)) {
        DCHIP("Failed to lock PCI device (exclusive)");
        goto fail_ipci;
    }
    pci_locked = TRUE;
    DCHIP("PCI device locked (exclusive)");

    /* --- Allocate ChipGPUState --- */
    gs = (struct ChipGPUState *)IExec->AllocVecTags(
        sizeof(struct ChipGPUState),
        AVT_Type,           MEMF_PRIVATE,
        AVT_ClearWithValue, 0,
        TAG_END);
    if (!gs) {
        DCHIP("Failed to allocate ChipGPUState");
        goto fail_pci;
    }

    gs->IExec     = IExec;
    gs->pciDevice = pciDev;
    gs->pci_locked = TRUE;
    gs->irq_installed = FALSE;
    /* io_lock MUST be recursive.  Callers like chip_flush / chip_flush_all /
     * chip_comp_CompositeTagList obtain io_lock to make a multi-command
     * GPU sequence atomic, then call chip_TransferToHost2D /
     * chip_ResourceFlush / chip_SetScanout, each of which re-acquires
     * io_lock internally.  A non-recursive mutex would self-deadlock the
     * flush task on its first iteration -- exactly what produced the
     * post-v53.110 black screen (the original SignalSemaphore was nestable,
     * the v53.110 mutex migration accidentally dropped that property).
     *
     * cursor_lock and alloc_mutex stay non-recursive: their critical
     * sections do not call into other locked code paths. */
    gs->io_lock = IExec->AllocSysObjectTags(ASOT_MUTEX,
        ASOMUTEX_Recursive, TRUE,
        TAG_END);
    gs->cursor_lock = IExec->AllocSysObjectTags(ASOT_MUTEX,
        ASOMUTEX_Recursive, FALSE,
        TAG_END);
    gs->alloc_mutex = IExec->AllocSysObjectTags(ASOT_MUTEX,
        ASOMUTEX_Recursive, FALSE,
        TAG_END);
    if (!gs->io_lock || !gs->cursor_lock || !gs->alloc_mutex) {
        DCHIP("io_lock/cursor_lock/alloc_mutex alloc failed");
        goto fail_gs;   /* fail_gs frees gs + all locks NULL-safely */
    }

    /* --- Enable PCI Memory Space + Bus Master EARLY (before BAR scan).
     * PCI_CFG fallback requires BARs to have active memory regions in QEMU.
     * Also disable INTx to prevent ISR-less interrupt crashes. --- */
    {
        uint16 pci_cmd = pciDev->ReadConfigWord(PCI_COMMAND);
        uint16 need = PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER | PCI_COMMAND_INT_DISABLE;
        if ((pci_cmd & need) != need) {
            pciDev->WriteConfigWord(PCI_COMMAND, pci_cmd | need);
            DCHIP("PCI COMMAND: 0x%04x -> 0x%04x (MEM+MASTER+INTDIS)",
                  (uint32)pci_cmd, (uint32)(pci_cmd | need));
        }
        uint32 caps = pciDev->GetCapabilities();
        if (!(caps & PCI_CAP_BUSMASTER))
            pciDev->SetCapabilities(PCI_CAP_BUSMASTER | PCI_CAP_SETCLR);
    }

    /* --- Scan all 6 PCI BARs -- log type/address/size, set IMMU on memory BARs --- */
    chip_scan_bars(gs);

    /* --- Scan PCI capabilities for VirtIO 1.0 config regions --- */
    if (!chip_scan_pci_caps(gs)) {
        DCHIP("Modern VirtIO caps not found (legacy not supported for GPU)");
        goto fail_gs;
    }

    /* --- Allocate DMA command/response buffers (4K each) --- */
    gs->cmd_buf = chip_dma_alloc(IExec, 4096, &gs->cmd_buf_phys);
    if (!gs->cmd_buf) {
        DCHIP("cmd_buf alloc failed");
        goto fail_gs;
    }
    gs->resp_buf = chip_dma_alloc(IExec, 4096, &gs->resp_buf_phys);
    if (!gs->resp_buf) {
        DCHIP("resp_buf alloc failed");
        goto fail_cmd;
    }

    /* Cursor command buffer -- separate from cmd_buf for lock-free cursor moves */
    gs->cursor_cmd_buf = chip_dma_alloc(IExec, 4096, &gs->cursor_cmd_phys);
    if (!gs->cursor_cmd_buf) {
        DCHIP("cursor_cmd_buf alloc failed (cursor will be software)");
        /* Non-fatal -- cursor just won't work */
    }

    DCHIP("cmd_buf phys=0x%08lx resp_buf phys=0x%08lx cursor_cmd phys=0x%08lx",
          gs->cmd_buf_phys, gs->resp_buf_phys, gs->cursor_cmd_phys);

    /* --- VirtIO 1.0 init (modern) --- */
    if (!chip_virtio_init(gs)) {
        DCHIP("VirtIO init failed");
        goto fail_resp;
    }

    /* --- Install IRQ handler.  Non-fatal: on failure, polling fallback. ---
     * Re-enable INTx (clear INT_DISABLE) only if IRQ install succeeds. */
    if (chip_irq_install(gs)) {
        uint16 pci_cmd = pciDev->ReadConfigWord(PCI_COMMAND);
        pciDev->WriteConfigWord(PCI_COMMAND, pci_cmd & ~PCI_COMMAND_INT_DISABLE);
        DCHIP("IRQ: INTx enabled, PCI_COMMAND=0x%04x -> 0x%04x",
              (uint32)pci_cmd, (uint32)(pci_cmd & ~PCI_COMMAND_INT_DISABLE));
    } else {
        DCHIP("IRQ: using polling fallback (INTx stays disabled)");
    }

    /* --- 3D: read num_capsets from device config, allocate cmd3d buffer --- */
    if (gs->has_virgl && gs->device_cfg_base) {
        /* Device config layout: events_read(4) + events_clear(4) + num_scanouts(4) + num_capsets(4) */
        gs->num_capsets = DEVCFG_R32(gs, 12);
        DCHIP("Device config: num_capsets=%lu", gs->num_capsets);

        /* Allocate 64K 3D command buffer for SUBMIT_3D */
        gs->cmd3d_buf = chip_dma_alloc(IExec, 64 * 1024, &gs->cmd3d_phys);
        if (gs->cmd3d_buf) {
            gs->cmd3d_lock = IExec->AllocSysObjectTags(ASOT_MUTEX,
                ASOMUTEX_Recursive, FALSE,
                TAG_END);
            if (!gs->cmd3d_lock) {
                DCHIP("WARNING: cmd3d_lock mutex alloc failed -- 3D disabled");
                chip_dma_free(IExec, gs->cmd3d_buf, 64 * 1024);
                gs->cmd3d_buf = NULL;
                gs->has_virgl = FALSE;
            } else {
                DCHIP("3D cmd buffer: virt=%p phys=0x%08lx (64K)",
                      gs->cmd3d_buf, gs->cmd3d_phys);
            }
        } else {
            DCHIP("WARNING: 3D cmd buffer alloc failed -- 3D disabled");
            gs->has_virgl = FALSE;
        }

        gs->next_resource_id = 3;  /* 1=fb, 2=cursor */
        gs->next_ctx_id = 1;
        gs->next_fence_id = 1;
    }

    /* --- Try EDID first for native resolution --- */
    uint32 width = 0, height = 0;
    BOOL got_edid = chip_GetEDID(gs, 0);
    DCHIP("EDID query: got_edid=%ld has_edid=%ld size=%lu native=%lux%lu",
          (LONG)got_edid, (LONG)gs->has_edid, gs->edid_size,
          gs->edid_native_w, gs->edid_native_h);
    if (got_edid && gs->has_edid && gs->edid_size >= 128) {
        /* Dump first 16 bytes of EDID for verification */
        DCHIP("EDID[0..15]: %02x %02x %02x %02x %02x %02x %02x %02x "
              "%02x %02x %02x %02x %02x %02x %02x %02x",
              gs->edid[0], gs->edid[1], gs->edid[2], gs->edid[3],
              gs->edid[4], gs->edid[5], gs->edid[6], gs->edid[7],
              gs->edid[8], gs->edid[9], gs->edid[10], gs->edid[11],
              gs->edid[12], gs->edid[13], gs->edid[14], gs->edid[15]);
    }
    if (got_edid && gs->edid_native_w > 0 && gs->edid_native_h > 0) {
        width  = gs->edid_native_w;
        height = gs->edid_native_h;
        DCHIP("Using EDID native resolution: %lux%lu", width, height);
    }

    /* --- Fallback: GET_DISPLAY_INFO --- */
    if (width == 0 || height == 0) {
        uint32 di_w = 0, di_h = 0;
        BOOL got_di = chip_GetDisplayInfo(gs, &di_w, &di_h);
        DCHIP("GET_DISPLAY_INFO: ok=%ld %lux%lu", (LONG)got_di, di_w, di_h);
        if (got_di && di_w > 0 && di_h > 0) {
            width  = di_w;
            height = di_h;
        } else {
            width  = 1024;
            height = 768;
            DCHIP("Using fallback %lux%lu", width, height);
        }
    }

    /* --- Max framebuffer dimensions = MaxHorResolution x MaxVerResolution ---
     * VirtIO GPU is virtual -- allocate fb at max supported resolution so
     * mode switches (from EDID or mode table) don't require resource
     * re-creation.  The "active" area is controlled by SetGC/SetScanout. */
    /* Match framebuffer resource to the EDID native (= initial active
     * mode).  Because QEMU's gl=on path sizes the SDL window to the
     * backing-texture (= resource) size and renders the whole texture
     * (egl_fb_blit ignores the scanout sub-rect), the resource MUST
     * equal the active scanout size for the SDL window and the AmigaOS
     * Workbench rendering to line up.  Mode changes recreate the
     * resource at the new active size (chip_resize_fb_for_mode). */
    uint32 max_w = width  ? width  : 1280;
    uint32 max_h = height ? height : 800;
    DCHIP("Max FB resolution: %lux%lu (EDID native: %lux%lu)", max_w, max_h, width, height);

    /* --- Allocate framebuffer at max resolution --- */
    gs->fb_width  = max_w;
    gs->fb_height = max_h;
    gs->fb_stride = max_w * 4;
    gs->fb_size   = max_h * gs->fb_stride;

    /* Allocate FB with MEMF_SHARED + page alignment, keep full DMA list */
    gs->fb_mem = IExec->AllocVecTags(gs->fb_size,
        AVT_Type,           MEMF_SHARED,
        AVT_Alignment,      4096,
        AVT_ClearWithValue, 0,
        TAG_END);
    if (!gs->fb_mem) {
        DCHIP("Framebuffer alloc failed (size=%lu)", gs->fb_size);
        goto fail_vqs;
    }

    gs->fb_dma_count = IExec->StartDMA(gs->fb_mem, gs->fb_size, DMA_ReadFromRAM);
    if (gs->fb_dma_count == 0) {
        DCHIP("FB StartDMA failed");
        goto fail_fb_mem;
    }

    gs->fb_dma_list = (struct DMAEntry *)IExec->AllocSysObjectTags(
        ASOT_DMAENTRY, ASODMAE_NumEntries, gs->fb_dma_count, TAG_DONE);
    if (!gs->fb_dma_list) {
        DCHIP("FB DMA list alloc failed");
        goto fail_fb_dma;
    }

    IExec->GetDMAList(gs->fb_mem, gs->fb_size, DMA_ReadFromRAM, gs->fb_dma_list);
    gs->fb_phys = (uint32)gs->fb_dma_list[0].PhysicalAddress;

    DCHIP("FB virt=0x%08lx phys=0x%08lx size=%lu (%lux%lu) DMA_ENTRIES=%lu",
          (uint32)gs->fb_mem, gs->fb_phys, gs->fb_size, max_w, max_h, gs->fb_dma_count);

    /* Set write-through on framebuffer for DMA coherency with store coalescing.
     * W=1 allows L2 cache to coalesce sequential pixel writes into 64-byte
     * blocks before flushing to memory, while ensuring data is always visible
     * in physical memory for TRANSFER_TO_HOST DMA reads.
     * Better than CI (every 4-byte write hits memory individually) and better
     * than normal cached (stale data risk for DMA reads).
     * Pattern from RadeonGCN-OS4 VRAM write-through strategy. */
    chip_immu_set_writethrough(IExec, gs->fb_mem, gs->fb_size);

    /* --- Double-buffering DISABLED for screen-resize debugging.
     * The front/back resource swap on every flush (including SET_SCANOUT
     * with the back buffer's resource ID) was complicating the SDL
     * window-size negotiation on screen-mode change.  Run single-buffer
     * until the resize/cursor alignment behaviour is fully understood. */
    gs->double_buffer = FALSE;
    gs->fb_mem2       = NULL;
    DCHIP("Double-buffering: DISABLED at compile-time (debug)");
    if (gs->fb_mem2) {
        gs->fb_dma_count2 = IExec->StartDMA(gs->fb_mem2, gs->fb_size, DMA_ReadFromRAM);
        if (gs->fb_dma_count2 > 0) {
            gs->fb_dma_list2 = (struct DMAEntry *)IExec->AllocSysObjectTags(
                ASOT_DMAENTRY, ASODMAE_NumEntries, gs->fb_dma_count2, TAG_DONE);
            if (gs->fb_dma_list2) {
                IExec->GetDMAList(gs->fb_mem2, gs->fb_size, DMA_ReadFromRAM, gs->fb_dma_list2);
                gs->fb_phys2 = (uint32)gs->fb_dma_list2[0].PhysicalAddress;
                chip_immu_set_writethrough(IExec, gs->fb_mem2, gs->fb_size);
                DCHIP("FB2 (double-buffer): virt=0x%08lx phys=0x%08lx (WT)",
                      (uint32)gs->fb_mem2, gs->fb_phys2);
            } else {
                IExec->EndDMA(gs->fb_mem2, gs->fb_size, DMA_ReadFromRAM | DMAF_NoModify);
                IExec->FreeVec(gs->fb_mem2);
                gs->fb_mem2 = NULL;
                DCHIP("FB2: DMA list alloc failed -- single-buffer mode");
            }
        } else {
            IExec->FreeVec(gs->fb_mem2);
            gs->fb_mem2 = NULL;
            DCHIP("FB2: StartDMA failed -- single-buffer mode");
        }
    } else {
        DCHIP("FB2: alloc failed (%luMB) -- single-buffer mode", gs->fb_size / (1024*1024));
    }

    /* --- Framebuffer resource (blob fast-path or classic 2D) --- */
    gs->resource_id    = 1;
    gs->fb_resource_id = 1;
    gs->fb_is_blob     = FALSE;

    /* If the host advertised VIRTIO_GPU_F_RESOURCE_BLOB, prefer the
     * BLOB_MEM_GUEST path: the host imports our DMA pages directly,
     * so RESOURCE_FLUSH alone is enough to refresh the screen -- no
     * TRANSFER_TO_HOST_2D round-trip per frame.  Saves ~4 MB / refresh
     * at 1280x800 32bpp, which is the dominant overhead at 60 Hz.
     *
     * Falls back to CREATE_2D + ATTACH_BACKING if the blob create
     * fails for any reason (older QEMU that advertises the feature
     * but doesn't fully support GUEST blobs, etc.). */
    BOOL fb_resource_ok = FALSE;
    if (gs->has_blob) {
        DCHIP("Trying BLOB_MEM_GUEST framebuffer (fast path): id=%lu size=%lu nr=%lu",
              gs->resource_id, gs->fb_size, gs->fb_dma_count);
        if (chip_ResourceCreateBlob(gs, gs->resource_id,
                                    VIRTIO_GPU_BLOB_MEM_GUEST,
                                    0,                /* no MAPPABLE -- we are the source */
                                    0,                /* blob_id only used for HOST3D */
                                    gs->fb_size,
                                    gs->fb_dma_list, gs->fb_dma_count)) {
            gs->fb_is_blob = TRUE;
            fb_resource_ok = TRUE;
            DCHIP("Framebuffer: BLOB_MEM_GUEST OK -- skipping TRANSFER_TO_HOST_2D on flush");
        } else {
            DCHIP("BLOB_MEM_GUEST create failed -- falling back to CREATE_2D");
        }
    }

    if (!fb_resource_ok) {
        DCHIP("RESOURCE_CREATE_2D: format=%lu (B8G8R8X8_UNORM=2) %lux%lu",
              (ULONG)VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM, max_w, max_h);
        if (!chip_ResourceCreate2D(gs, gs->resource_id,
                                    VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM, max_w, max_h)) {
            DCHIP("RESOURCE_CREATE_2D failed");
            goto fail_fb_dma_list;
        }
        if (!chip_ResourceAttachBacking(gs, gs->resource_id,
                                         gs->fb_dma_list, gs->fb_dma_count)) {
            DCHIP("RESOURCE_ATTACH_BACKING failed");
            goto fail_fb_dma_list;
        }
    }

    /* --- Create second 2D resource for double-buffering --- */
    if (gs->fb_mem2) {
        gs->fb_resource_id2 = 9;  /* fixed ID, distinct from fb(1), cursor(2), virgl(3+) */
        if (chip_ResourceCreate2D(gs, gs->fb_resource_id2,
                                    VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM, max_w, max_h) &&
            chip_ResourceAttachBacking(gs, gs->fb_resource_id2,
                                         gs->fb_dma_list2, gs->fb_dma_count2)) {
            gs->double_buffer = TRUE;
            DCHIP("Double-buffering: ENABLED (res1=%lu res2=%lu)",
                  gs->fb_resource_id, gs->fb_resource_id2);
        } else {
            DCHIP("Double-buffering: 2D resource creation failed -- single-buffer mode");
            gs->fb_resource_id2 = 0;
        }
    }

    /* --- SET_SCANOUT at EDID/initial resolution (active area) --- */
    if (!chip_SetScanout(gs, 0, gs->resource_id, 0, 0, width, height)) {
        DCHIP("SET_SCANOUT failed");
        goto fail_fb_dma_list;
    }

    /* --- Initial flush: send blank (zeroed) fb to GPU --- */
    chip_TransferToHost2D(gs, gs->resource_id, 0, 0, width, height, 0ULL);
    chip_ResourceFlush(gs, gs->resource_id, 0, 0, width, height);

    /* --- Allocate board memory (separate from GPU framebuffer) ---
     * graphics.library draws into board_mem (CLUT 8bpp or TrueColor 32bpp).
     * On flush, we convert board_mem -> fb_mem (always 32bpp for GPU).
     * 128MB for 4K 32bpp (33MB) + off-screen bitmaps + future 3D headroom. */
    gs->board_mem_size = 128 * 1024 * 1024;
    gs->board_mem = IExec->AllocVecTags(gs->board_mem_size,
        AVT_Type,           MEMF_SHARED,
        AVT_Alignment,      4096,
        AVT_ClearWithValue, 0,
        TAG_END);
    if (!gs->board_mem) {
        DCHIP("Board memory alloc failed (size=%lu)", gs->board_mem_size);
        /* Fall back: use fb_mem as both board and GPU memory (32bpp only) */
        gs->board_mem      = gs->fb_mem;
        gs->board_mem_size = gs->fb_size;
        DCHIP("Fallback: board_mem = fb_mem");
    } else {
        DCHIP("Board mem virt=0x%08lx size=%lu", (uint32)gs->board_mem, gs->board_mem_size);
    }
    gs->active_format  = RGBFB_A8R8G8B8;  /* default until SetGC */
    gs->active_bpr     = width * 4;
    gs->active_width   = width;
    gs->active_height  = height;
    gs->panning_mem    = gs->board_mem;
    gs->cursor_x       = -1;
    gs->cursor_y       = -1;
    gs->cursor_w       = 16;
    gs->cursor_h       = 16;

    /* --- Store state globally for vtable callbacks --- */
    g_chip_state = gs;
    gs->bi = bi;

    /* Vsync rendezvous mutex.  Non-recursive: chip_WaitVerticalSync
     * acquires once (publish), flush task acquires once (snapshot +
     * clear), neither nests.  Allocation failure is non-fatal --
     * chip_WaitVerticalSync detects the NULL and returns immediately,
     * matching the canonical "no vsync hardware" behaviour. */
    gs->vsync_mutex = IExec->AllocSysObjectTags(ASOT_MUTEX,
        ASOMUTEX_Recursive, FALSE,
        TAG_END);
    if (!gs->vsync_mutex) {
        DCHIP("vsync_mutex alloc failed -- WaitVerticalSync will be a no-op");
    }

    /* --- Fill BoardInfo --- */

    /* bi->CardBase and bi->ChipBase are set by hook_InitBoard (chip_board.c)
     * BEFORE this function is called.  Do NOT overwrite them here.
     * bi->ExecBase and bi->UtilBase are set by PCIGraphics.card.
     * We DO override MemoryBase/MemorySize/MemoryIOBase (see below)
     * to point at our VirtIO GPU DMA framebuffer. */

    /* Board identification.
     *   BoardName: shown in screen-mode prefs and gfxbench2d output.
     *   GraphicsControllerType: our own BT_VIRTIOGPU (not BT_uaegfx; we are
     *       not UAE emulation).  Tools that display this value will show
     *       "0x56" or unknown -- acceptable; rtg.library does not gate any
     *       behaviour on this field.
     *   PaletteChipType: 14 matches the SM502.chip pattern and is what
     *       graphics.library expects for CLUT-capable modern boards.
     *   MoniSwitch: 1 = "VGA connector".  Virtual, but required for
     *       the display chain to initialise.
     *   MemoryClock: 200 MHz -- a display-only indicator; not meaningful
     *       on a virtual device. */
    bi->BoardName              = (STRPTR)"VirtIOGPU";
    /* Board / chip / DAC identification.  These are all used for
     * diagnostic display only -- rtg.library and graphics.library do
     * not gate behaviour on the values.  Previous versions put
     * BT_VIRTIOGPU into GraphicsControllerType (wrong slot) and used
     * PCT_TIPermedia2 (=14) for the DAC, which caused diagnostic tools
     * to report a Permedia2 palette chip we don't have. */
    bi->BoardType              = BT_VIRTIOGPU;  /* @170 BTYPE  */
    bi->PaletteChipType        = 0;             /* @174 PCT_Unknown */
    bi->GraphicsControllerType = 0;             /* @178 GCT_Unknown */
    bi->MoniSwitch             = 1;
    bi->MemoryClock            = 200000000;
    /* VBI handler name -- shown by debugging tools that walk
     * IExec->IntServer chains.  Embedded char[32]; pad with NUL. */
    {
        const char *vbi = "VirtIOGPU.vbi";
        char *dst = bi->VBIName;
        ULONG i;
        for (i = 0; i < sizeof(bi->VBIName) - 1 && vbi[i]; i++) dst[i] = vbi[i];
        for (; i < sizeof(bi->VBIName); i++) dst[i] = '\0';
    }

    /* Memory layout.
     * PCIGraphics.card pre-fills these with SM502 PCI BAR addresses:
     *   bi->MemoryBase   = 0x80000000 (SM502 BAR0 -- not our framebuffer!)
     *   bi->MemoryIOBase = 0x00000000
     *   bi->MemorySize   = 0x04000000 (64MB)
     * We MUST override MemoryBase to point to our DMA framebuffer so P96
     * draws directly into the buffer VirtIO GPU reads from.  The old
     * "overwriting MemoryBase crashes" diagnosis was wrong -- the real
     * crash was from uninitialized HardInterrupt (fixed in v53.24). */
    bi->MemoryBase   = (APTR)gs->board_mem;
    bi->MemorySize   = gs->board_mem_size;
    bi->MemoryIOBase = NULL;
    /* Mirror to canonical MemorySpaceBase/MemorySpaceSize (+1546/+1550).
     * RadeonRX.chip writes the same MemoryBase/MemorySize values to
     * those offsets in InitChip; following the same convention lets
     * tools that walk the canonical fields read meaningful values. */
    boardinfo_set_memory_space(bi, (APTR)gs->board_mem, gs->board_mem_size);

    /* Mode capability tables -- MAXMODES=5:
     * 0=PLANAR, 1=CHUNKY(8bpp), 2=HICOLOR(16bpp), 3=TRUECOLOR(24bpp), 4=TRUEALPHA(32bpp)
     *
     * SM502 pattern: only populate mode classes that match RGBFormats.
     * We support CHUNKY (8bpp CLUT), HICOLOR (16bpp R5G6B5), and TRUEALPHA (32bpp).
     * Leave unsupported classes (0, 2, 3) at zero -- graphics.library skips them.
     *
     * MaxHorValue/MaxVerValue = max CRTC timing total (active + blanking + sync).
     * MaxHorResolution/MaxVerResolution = max visible pixels.
     * PixelClockCount = number of pixel clock entries in mode table. */

    /* CHUNKY -- 8bpp CLUT */
    bi->MaxHorValue[1]      = 4096;
    bi->MaxVerValue[1]      = 4096;
    bi->MaxHorResolution[1] = 4096;
    bi->MaxVerResolution[1] = 4096;
    bi->PixelClockCount[1]  = chip_num_modes;

    /* HICOLOR -- 16bpp */
    bi->MaxHorValue[2]      = 4096;
    bi->MaxVerValue[2]      = 4096;
    bi->MaxHorResolution[2] = 4096;
    bi->MaxVerResolution[2] = 4096;
    bi->PixelClockCount[2]  = chip_num_modes;

    /* TRUEALPHA -- 32bpp */
    bi->MaxHorValue[4]      = 4096;
    bi->MaxVerValue[4]      = 4096;
    bi->MaxHorResolution[4] = 4096;
    bi->MaxVerResolution[4] = 4096;
    bi->PixelClockCount[4]  = chip_num_modes;

    /* Flags -- GRANTDIRECTACCESS lets P96 write directly to our board_mem buffer
     * (it's regular RAM). We flush to VirtIO GPU in SetSwitch/SetPanning/etc.
     * INDISPLAYCHAIN = this board is the active display output (required for
     * graphics.library to fully init display structures after SetDisplay(1)).
     * HARDWARESPRITE = cursor via VirtIO GPU cursor plane (queue 1), independent
     * of framebuffer flush -- instant movement via CURSOR_MOVE command.
     * BLITTER = FillRect/BlitRect are GPU-accelerated via Virgl (CLEAR/BLIT)
     * when the host advertises VIRGL.  Diagnostic tools and a few legacy apps
     * gate accelerated paths on this bit; advertising it is correct in either
     * mode (Virgl: GPU-side, no-Virgl: CPU loops still beat planar fallback). */
    /* Bisected against v53.103 working baseline + v53.104 additions --
     * see VERSIONS.md.  Result:
     *
     *   BIF_BLITTER          BLACK SCREEN.  Setting this bit tells
     *     graphics.library V54 that the chip handles ALL 2D drawing
     *     via its 5 FPs (WaitBlitter, FillRect, InvertRect, BlitRect,
     *     BlitRectNoMaskComplete).  graphics.library then routes the
     *     compositor's "blit off-screen window content onto the screen"
     *     step through our chip vtable -- but its compositor path
     *     evidently never invokes BlitRectNoMaskComplete with the
     *     screen bitmap as destination, so Workbench BG never reaches
     *     panning_mem.  Without the bit, graphics.library uses direct
     *     CPU writes to bi->MemoryBase for the same path, bypassing
     *     the vtable, and the flush task picks up the pixels.
     *
     *     Re-enable only when a full chip-side rendering pipeline is
     *     available (e.g. Virgl negotiated -- chip_virgl_2d.c's CLEAR/
     *     BLIT path), AND only after confirming the compositor's
     *     screen-blit step uses our chip vtable correctly.
     *
     *   BIF_HASSPRITEBUFFER  Safe.  Stops rtg.library allocating an
     *     unused MouseSaveBuffer; the cursor save/restore path on the
     *     framebuffer still works (verified by NMCDIAG).
     *
     *   BIF_VBLANKINTERRUPT  Safe.  Advertises that we drive bi->WaitQ
     *     via Cause(bi->SoftInterrupt) on vblank.  Even with our
     *     synthesised vblank, no AOS4 component blocked on this in
     *     bisect testing.
     */
    bi->Flags         = BIF_GRANTDIRECTACCESS | BIF_NOMASKBLITS | BIF_NOC2PBLITS
                      | BIF_INDISPLAYCHAIN | BIF_HARDWARESPRITE
                      | BIF_HASSPRITEBUFFER | BIF_VBLANKINTERRUPT;
    /* HASSPRITEBUFFER: VirtIO cursor plane is a permanent 64x64 ARGB
     * resource on the GPU, completely independent of the framebuffer.
     * Setting this tells rtg.library not to allocate a host-side save
     * buffer (MouseSaveBuffer) -- there is nothing to save/restore
     * since the cursor never paints into our backing memory. */
    /* SoftSpriteFlags = 0: hardware cursor for all formats.  VirtIO GPU cursor
     * plane is format-independent (always 64x64 B8G8R8A8). */
    bi->SoftSpriteFlags = 0;
    bi->ChipFlags       = 0;
    bi->CardFlags       = 0;

    /* Format support -- A8R8G8B8 is the native AmigaOS 4 32bpp format.
     * GPU resource uses X8R8G8B8_UNORM which has identical byte layout
     * on PPC BE, so A8R8G8B8 data passes through with zero conversion. */
    bi->RGBFormats    = (WORD)(RGBFF_CLUT | RGBFF_R5G6B5 | RGBFF_A8R8G8B8);
    bi->BitsPerCannon = 8;
    bi->RGBFormat     = RGBFB_A8R8G8B8;
    bi->Depth         = 32;

    /* RTG output is on by default -- chip_SetSwitch(FALSE) will park
     * us if graphics.library reroutes the display chain elsewhere. */
    gs->rtg_output_enabled = TRUE;

    /* Drawing mask: 0xFFFFFFFF means "all bits valid in writes".  The
     * default 0 from PCIGraphics.card's allocation reads as "no bits
     * valid", which can confuse SetWriteMask-aware paths.  For our
     * 32bpp default we want the canonical "all on". */
    bi->Mask          = 0xFFFFFFFFUL;

    /* Border colour disabled by default -- screen border (the area
     * around an underscanned mode) stays the screen's BgPen.  Toggled
     * by graphics.library when the user sets BORDERBLANK in display
     * preferences.  Explicit init for clarity. */
    bi->Border        = FALSE;

    /* Stash PCI Vendor/Device IDs at +1566 / +1568 -- the location AOS4
     * PCIGraphics.card uses for GBD_PCIVendorID / GBD_PCIProductID.
     * Our SetMethod injection bypasses PCIGraphics.card's own scan, so
     * if we don't write these the queries return 0/0.  IDs are the
     * VirtIO GPU PCI device class. */
    boardinfo_set_pci_id(bi, 0x1AF4, 0x1050);

    /* Zero-init SyncTime (+1558 / +1562) -- the "system time when
     * screen was set up" baseline for pseudo-vblank arithmetic.  We
     * leave it at 0/0 because (a) the matching SyncPeriod slot at
     * +1566 is repurposed for PCI IDs in AOS4 so the pseudo-vblank
     * computation is broken at the API level anyway, (b) timer.device
     * isn't open at this point in init so CurrentTime() can't give us
     * a meaningful value, and (c) explicit zero is clearer than stale
     * uninitialised data. */
    boardinfo_set_sync_time(bi, 0, 0);

    /* NOTE: do NOT write bi+1616 (canonical EssentialFormats), bi+1642
     * (canonical MaxBMWidth) or bi+1646 (canonical MaxBMHeight) on AOS4.
     * graphics.library.kmod disassembly shows it uses bi+1646 as the
     * head of a singly-linked hash chain (BitMapExtra lookups) -- the
     * pattern at gfx 0x104db5c is the classic "insert at head" idiom:
     *     r10 = *(bi+1646); *(node+18) = r10; *(bi+1646) = node
     * Writing 4096 there would corrupt the chain.  bi+1642 is also
     * read as a pointer in mouse-positioning paths, not as a dimension.
     * bi+1616 is OR-accumulated bit-by-bit by a graphics.library
     * routine, possibly indistinguishable from a list head.  AOS4 has
     * reorganised this whole region away from canonical so the canonical
     * field names cannot be trusted. */

    /* ----------------------------------------------------------------
     * Initialise exec structures in BoardInfo.
     *
     * DO NOT touch BoardLock, ResolutionsList, or any other runtime/
     * management fields -- PCIGraphics.card allocates and pre-initialises
     * the entire BoardInfo struct (including AddSemaphore(&bi->BoardLock))
     * BEFORE calling chip_FindCard.  Calling InitSemaphore here would
     * zero ss_Link.ln_Succ on an already-queued semaphore node, corrupting
     * exec's semaphore list -> DAR=0x00000000 DSI crash at kernel+0x1B99C.
     *
     * DO NOT touch SpecialFeatures, BitMapList, MemList, or any other
     * runtime/management fields -- rtg.library manages those.
     * ---------------------------------------------------------------- */

    /* Fill vtable (68 entries, starts at offset 274 in BoardInfo) */
    chip_fill_boardinfo_vtable(bi);

    /* BoardInfo fields that feed graphics.library V54 GetBoardDataTagList:
     *   GBD_BoardName       <- bi->BoardName       ("VirtIOGPU")
     *   GBD_TotalMemory     <- bi->MemorySize      (board RAM bytes)
     *   GBD_BoardDriver     <- "PCIGraphics.card"  (hard-coded by the card)
     *   GBD_ChipDriver      <- "virtiogpu.chip"    (our Resident name)
     *   GBD_FreeMemory / GBD_LargestFreeMemory    <- tracked by our bump
     *       allocator in chip_p96.c (AllocCardMem) -- graphics.library
     *       reads these by iterating the P96 memory list we maintain.
     *   GBD_PCIVendorID / GBD_PCIProductID: these read the UWORD pair
     *       at bi+1566 / bi+1568 in PCIGraphics.card's extended area;
     *       boardinfo_set_pci_id above writes our 0x1AF4 / 0x1050 there
     *       so V54 GBD queries return the correct values. */
    DCHIP("InitCard SUCCESS: %lux%lu MemBase=%p MemSize=%lu Flags=0x%lx RGBFormats=0x%04lx",
          width, height, bi->MemoryBase, (ULONG)bi->MemorySize,
          bi->Flags, (ULONG)(UWORD)bi->RGBFormats);

    /* Register EDID callback at bi+1714 for graphics.library v54+ mode
     * discovery.  graphics.library calls ConfigureDisplay to obtain EDID
     * data, then builds its own mode database.  Pattern from SM502.chip. */
    chip_register_edid(gs, bi);

    /* Register screen modes from chip_modes[] table + optional EDID native */
    chip_register_modes(gs);

    /* Query Virgl capsets (after DRIVER_OK, mode registration) */
    if (gs->has_virgl && gs->num_capsets > 0) {
        chip_QueryCapsets(gs);
    }

    /* Virgl 2D acceleration DISABLED for screen-resize debugging.
     * Without Virgl 2D, the scanout target stays as fb_resource_id=1
     * (a classic 2D resource sized at MAX width x height), which is
     * created once and never destroyed.  Screen-mode changes just call
     * SET_SCANOUT with the new active rect inside the same resource --
     * no resource teardown, no context recovery, no pipeline rebuild.
     * This isolates the screen-mode resize behaviour from all the Virgl
     * recovery machinery. */
    DCHIP("Virgl 2D acceleration: DISABLED at compile-time (debug)");

    /* Flush task is NOT created here -- dos.library is unavailable during
     * early boot (chip init runs at pri 65, DOS at pri -120).
     * Deferred to first SetSwitch(TRUE) call when the system is ready. */
    gs->flush_task_quit = FALSE;

    return TRUE;

    /* --- Error cleanup (goto targets, reverse resource acquisition order) ---
     * Each label frees resources acquired up to that point.  Use chip_dma_free
     * (NULL-safe) for all StartDMA'd buffers to ensure EndDMA is paired.
     * Optional buffers (cursor_cmd_buf, cmd3d_buf) are unconditionally freed
     * via the NULL-safe helper -- they stay NULL if their alloc was skipped. */
fail_fb_dma_list:
    /* Second framebuffer (double-buffer back) -- NULL-safe.  fb_mem2,
     * fb_dma_list2 are allocated best-effort earlier; if they
     * succeeded, every late failure path lands here and must clean up.
     * The internal fb_mem2 unwind (line ~990) handles only failures
     * during its own allocation. */
    if (gs->fb_dma_list2) {
        IExec->FreeSysObject(ASOT_DMAENTRY, gs->fb_dma_list2);
        gs->fb_dma_list2 = NULL;
    }
    if (gs->fb_mem2) {
        IExec->EndDMA(gs->fb_mem2, gs->fb_size, DMA_ReadFromRAM | DMAF_NoModify);
        IExec->FreeVec(gs->fb_mem2);
        gs->fb_mem2 = NULL;
    }
    /* Vsync mutex -- safe to free here: no task can be holding it
     * because the chip vtable was never published (we're failing
     * chip_InitCard_C). */
    if (gs->vsync_mutex) {
        IExec->FreeSysObject(ASOT_MUTEX, gs->vsync_mutex);
        gs->vsync_mutex = NULL;
    }
    IExec->FreeSysObject(ASOT_DMAENTRY, gs->fb_dma_list);
fail_fb_dma:
    IExec->EndDMA(gs->fb_mem, gs->fb_size, DMA_ReadFromRAM | DMAF_NoModify);
fail_fb_mem:
    IExec->FreeVec(gs->fb_mem);
    /* cmd3d_buf is allocated after fb_mem is alloc'd (line ~818) but before
     * any late failure.  Safe to free here via NULL-check. */
    chip_dma_free(IExec, gs->cmd3d_buf, 64 * 1024);
fail_vqs:
    if (gs->vqs[0]) VirtQueue_Free(IExec, gs->vqs[0]);
    if (gs->vqs[VIRTIO_GPU_CURSQ]) VirtQueue_Free(IExec, gs->vqs[VIRTIO_GPU_CURSQ]);
fail_resp:
    /* cursor_cmd_buf is allocated between resp_buf and chip_virtio_init --
     * NULL-safe free covers the case where it wasn't allocated. */
    chip_dma_free(IExec, gs->cursor_cmd_buf, 4096);
    chip_dma_free(IExec, gs->resp_buf, 4096);
fail_cmd:
    chip_dma_free(IExec, gs->cmd_buf, 4096);
fail_gs:
    /* Mutexes -- NULL-safe.  cmd3d_lock is only allocated when virgl
     * negotiation succeeds; the other two are allocated very early
     * (right after gs).  Every late failure path falls through here
     * so cleanup is automatic. */
    if (gs->cmd3d_lock) {
        IExec->FreeSysObject(ASOT_MUTEX, gs->cmd3d_lock);
        gs->cmd3d_lock = NULL;
    }
    if (gs->cursor_lock) {
        IExec->FreeSysObject(ASOT_MUTEX, gs->cursor_lock);
        gs->cursor_lock = NULL;
    }
    if (gs->alloc_mutex) {
        IExec->FreeSysObject(ASOT_MUTEX, gs->alloc_mutex);
        gs->alloc_mutex = NULL;
    }
    if (gs->io_lock) {
        IExec->FreeSysObject(ASOT_MUTEX, gs->io_lock);
        gs->io_lock = NULL;
    }
    IExec->FreeVec(gs);
fail_pci:
    if (pci_locked) pciDev->Unlock();
fail_ipci:
    IExec->DropInterface((struct Interface *)IPCI);
fail_exp:
    IExec->CloseLibrary(ExpBase);
    return FALSE;
}
