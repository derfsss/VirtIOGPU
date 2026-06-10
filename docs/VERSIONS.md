# VirtIOGPU — Version History

## v1.0 — 05.03.2026 (build 1)

**Phase 1: Basic Framebuffer**

Initial release. Driver skeleton based on VirtIODeviceBase template, adapted
for VirtIO GPU (PCI 1AF4:1050).

### New features

- PCI device discovery for VirtIO GPU modern device (ID 0x1050)
- VirtIO 1.0 modern protocol: capability scan, feature negotiation, MMIO init
- 2 virtqueues: queue 0 (control), queue 1 (cursor)
- Feature negotiation: accepts `VIRTIO_GPU_F_EDID` (bit 1)
- `VIRTIO_GPU_CMD_GET_DISPLAY_INFO` — queries active scanouts and dimensions
- Per-scanout framebuffer allocation:
  - `MEMF_SHARED` linear 32bpp (B8G8R8X8_UNORM) framebuffer
  - DMA-mapped for physical address
  - `RESOURCE_CREATE_2D` + `RESOURCE_ATTACH_BACKING` + `SET_SCANOUT`
  - Initial blank flush via `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH`
- `virtiogpu.device`: standard AmigaOS device driver interface
  - One unit per enabled scanout (unit 0 = first display)
  - `NSCMD_DEVICEQUERY` supported inline
  - Per-unit device task (currently dispatches no commands)
  - `RTF_COLDSTART` at priority -50 (loads before mounter.library)
- GPU control layer (`gpu_ctrl.c`): synchronous wrappers for all 2D commands
- Display lifecycle (`gpu_display.c`): init, flush, shutdown per scanout
- No clib4 dependency — all AmigaOS SDK APIs

### Known limitations

- No AmigaOS graphics.library integration (no RTG, no Workbench)
- No incremental flush command (full re-display only at init time)
- No cursor support
- AmigaOne (Articia S) not supported
- Single framebuffer only (no double buffering)

---

## v1.1 — 05.03.2026 (builds 3–8)

**Phase 1+2: First pixels on screen (CONFIRMED WORKING)**

### Fixes

- **Build 3**: Added `-DDEBUG`; `DebugPrintF` on all VirtIO failure paths.
- **Build 4**: PCI_CFG capability (type 5) access path for non-MMIO platforms.
- **Build 5**: Guard PCI_CFG cap on BAR availability — QEMU asserts on absent BARs.
- **Build 6**: Fixed GPU protocol byte-order. All `virtio_gpu_*` struct fields are
  little-endian; PPC is big-endian. Every field goes through `GP32`/`GP16`/`GP64`.
- **Build 7**: Fixed init-time interrupt signal. Added `init_wait_task`/`init_signal_mask`
  to `VirtIOGPUBase`; ISR signals init task as fallback during driver Init.
- **Build 8**: Fixed signal bit mismatch. `VirtIO_DoIO` now reuses `init_signal_mask`
  in init context. **Result: colour bars confirmed on screen — Phase 1 working.**

---

## v1.1 — 05.03.2026 (build 2, Phase 2)

**Phase 2: Flush Command + Hardware Cursor**

### New features

- `GPU_CMD_QUERY_FB` (0x8000) — fills `struct GPU_FBInfo`
- `GPU_CMD_FLUSH` (0x8001) — dirty rectangle flush via `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH`
- `GPU_CMD_MOVE_CURSOR` (0x8002) — cursor position via cursor queue
- `GPU_CMD_SET_CURSOR_IMAGE` (0x8003) — 64×64 BGRA cursor bitmap upload
- `GPU_CMD_SET_CURSOR_VISIBLE` (0x8004) — show/hide cursor
- Public header `include/gpu/gpu_cmds.h`
- Test program `tests/test_gpu`

---

## v2.0 — 05.03.2026 (build 1)

**Phase 3 attempt: Picasso96 chip driver (virtiogpu.chip)**

VERSIONS.md described this phase as complete, but actual boot testing
revealed significant issues still being resolved. See below for actual status.

---

## v2.0 — In Progress (08.03.2026)

**Phase 3: PCIGraphics.card chip driver — SetMethod hook approach**

### Architecture (revised — SetMethod hooks, not SM502 masquerade)

The chip driver now uses a two-resident approach (same pattern as RadeonRX.chip):

1. **VirtIOGPUBoard** (NT_RESOURCE, pri 65) — runs before graphics.library (pri 63):
   - Scans PCI for VirtIO GPU (1AF4:1050)
   - Force-inits PCIGraphics.card via FindResident + InitResident
   - Hooks FindCard (offset 76) and InitBoard (offset 80) via SetMethod
   - Injects VirtIO GPU PCI device at bi+1630 in hook_FindCard
   - Opens virtiogpu.chip library and calls HardwareInit in hook_InitBoard

2. **virtiogpu.chip** (NT_LIBRARY, pri -128) — loaded on demand:
   - chip_FindCard fills bi->HardInterrupt, calls chip_InitCard_C
   - chip_InitCard_C does full embedded VirtIO GPU init + BoardInfo fill

### Source file layout (refactored 08.03.2026)

The monolithic `virtiogpu_chip.c` (2790 lines) was split into 7 files + 1 header:

| File | Lines | Purpose |
|------|-------|---------|
| `include/chip/chip_state.h` | ~280 | Shared header: ChipGPUState, MMIO inlines, macros |
| `src/chip/chip_lib.c` | ~225 | Library boilerplate, ROM tags, _start asm |
| `src/chip/chip_board.c` | ~320 | VirtIOGPUBoard resident, SetMethod hooks |
| `src/chip/chip_init.c` | ~750 | PCI/VirtIO init, chip_InitCard_C (goto cleanup) |
| `src/chip/chip_gpu_cmds.c` | ~290 | GPU command wrappers (polling I/O) |
| `src/chip/chip_flush.c` | ~170 | Flush, convert, periodic flush task |
| `src/chip/chip_p96.c` | ~620 | All 68 P96 BoardInfo vtable functions |
| `src/chip/chip_modes.c` | ~180 | Screen mode table + registration |

### Lessons learned

- **cardDesc must NOT be called** — it's a PCIGraphics.card C++ object with a short
  vtable; indexing past it hits 0xFFFFFFFF sentinel → ISI crash
- **bi->HardInterrupt must be filled BEFORE returning non-NULL** from chip_FindCard
- **IMMU CI+G on memory BARs** required (pattern from RadeonRX.chip at 0x101ab54)
- **PCI_COMMAND_INT_DISABLE** before DRIVER_OK prevents ISR-less interrupt crashes
- **`goto cleanup` pattern** eliminates duplicated error cleanup code in chip_InitCard_C

### Build

```
CHIP_CFLAGS  = -O2 -Wall -I./include -fno-tree-loop-distribute-patterns -DDEBUG \
               -mcrt=newlib -D__NOLIBBASE__ -D__NOGLOBALIFACE__
CHIP_LDFLAGS = -mcrt=newlib -nostartfiles
```

Output: `build/virtiogpu.chip`.

---

## v53.64 — 09.03.2026

**Phase 3: Ghost rectangle fix + strip-based flush**

### Fixes

- **DrawMode bitwise check**: P96 DrawMode field contains extra flags in high bits
  (e.g. 0x82 = COMPLEMENT | 0x80). All draw mode checks in DrawLine, BlitTemplate,
  and BlitPattern used exact equality (`dm == COMPLEMENT`) which failed when extra
  flags were set. Fixed to use bitwise tests (`dm & COMPLEMENT`). This caused ghost
  rubber-band selection rectangles on Workbench — XOR draw/erase never executed,
  so lines were written with solid fg colour and never erased.

### Improvements

- **Strip-based flush_all**: Full-screen flush now processes 64-row strips with
  per-strip io_lock acquire/release, reducing lock hold time and allowing chip_flush
  (from P96 drawing ops) to interleave between strips.

---

## v53.65 — 09.03.2026

**Phase 4+5: 16bpp, EDID, DPMS, Virgl 2D acceleration**

### New features

- **16bpp R5G6B5 support**: Full 16bpp hi-color mode including:
  - R5G6B5 -> B8G8R8X8_UNORM conversion in flush pipeline (chip_flush.c)
  - 16bpp pixel ops in FillRect, BlitTemplate, BlitPattern, DrawLine
  - HICOLOR mode class (index 2) registered with MaxHorValue/MaxVerValue 4096
  - Screen modes created at 16bpp depth for all resolutions
  - GetCompatibleFormats, RGBFormats, SoftSpriteFlags updated

- **DPMS power management**: SetDPMSLevel uses SET_SCANOUT with resource_id=0
  to blank display on STANDBY/SUSPEND/OFF, re-enables on DPMS_ON.

- **Activity-based idle skip**: Flush task tracks drawing activity via
  activity_counter. Skips flush when screen is idle (>500ms since last draw),
  with 1s heartbeat. Reduces CPU usage ~98% when idle.

- **VirtIO GPU config change events**: Flush task polls device_cfg events_read
  every ~1s. On VIRTIO_GPU_EVENT_DISPLAY, queries new dimensions and updates
  scanout if within pre-allocated resource bounds.

- **Virgl 2D acceleration**: GPU-accelerated drawing operations via Virgl 3D context:
  - 3D resource as scanout (replaces 2D resource when Virgl available)
  - FillRect via Virgl CLEAR with scissor — GPU fills rect directly
  - BlitRect via Virgl BLIT — GPU copies within same resource, handles overlap
  - TRANSFER_TO_HOST_3D flush path for GRANTDIRECTACCESS background writes
  - Pipeline state: blend + DSA + rasterizer (with scissor) + surface + framebuffer
  - No shaders needed — CLEAR/BLIT/TRANSFER don't require vertex/fragment programs
  - Supports 32bpp, 16bpp (R5G6B5→float conversion), and CLUT (palette lookup)

- **New source files**:
  - `chip_gpu_3d.c` — 3D GPU commands (CTX_CREATE, SUBMIT_3D, TRANSFER_TO_HOST_3D)
  - `chip_virgl.c` — Virgl capability query and feature negotiation
  - `chip_virgl_2d.c` — Virgl 2D init/shutdown/fill_rect/blit_rect
  - `virgl_cmd.h` — Virgl command buffer builder API and pipe constants

### Fixes

- **QEMU crop blit Y-flip**: Fixed `sdl2_gl_blit_crop` in local QEMU build —
  didn't flip Y for y0_top=false when 3D resource > scanout size.

### Known issues

- **VIRGL_CCMD_CLEAR ignores scissor**: Gallium `pipe->clear()` unconditionally
  disables GL_SCISSOR_TEST. FillRect works due to flush_all masking.

---

## v53.71 — 10.03.2026

**Phase 5b: DRAW_VBO 3D Rendering Working**

First successful Virgl DRAW_VBO rendering — colored quad visible on Workbench.

### Bug fixes

- **Rasterizer S0 bit layout corrected**: Scissor was at bit 3 (actually
  `rasterizer_discard`) — all DRAW_VBO output was silently discarded. Fixed:
  scissor is bit 14, cull_face bits 8-9, fill_front bits 10-11, fill_back bits 12-13.

- **CLEAR payload length 9→8**: Header declared 9 payload words but only 8 were
  emitted. Caused context poisoning (EINVAL) when CLEAR wasn't the last command
  in a submit. Previously hidden because FillRect always had CLEAR as last command.

### New features

- **DRAW_VBO rendering**: Vertex upload via RESOURCE_INLINE_WRITE, passthrough
  vertex shader + color fragment shader, DRAW_VBO with PIPE_PRIM_TRIANGLES.
  Renders colored quads at pixel-precise coordinates via NDC conversion.

- **TGSI shaders working**: VS, color FS, and texture FS all create successfully
  (fixed in v53.67 shader encoding, confirmed working in v53.71).

- **Full pipeline state**: blend, DSA, rasterizer (scissor+depth_clip), shaders,
  vertex elements, vertex buffer resource, samplers (nearest+linear) — all bound
  and operational.

### Lessons learned

- `VIRGL_CCMD_CLEAR` is Gallium's full-framebuffer clear — scissor is always
  disabled. For partial fills, use DRAW_VBO or VIRGL_CCMD_CLEAR_TEXTURE (cmd 47).
- `virgl_emit_dword` uses GP32 byte-swap — all Virgl command buffer data is
  correctly encoded in LE wire format. No separate byte-swap needed.

---

## v53.72 — 10.03.2026

**Phase 5c: Double-Buffering for Tear-Free Display**

### New features

- **Double-buffering**: Two GPU resources (front/back) with separate DMA-backed
  framebuffers. flush_all renders to the back buffer, then atomically swaps via
  SET_SCANOUT + RESOURCE_FLUSH. Eliminates tearing from partial front updates.
- **chip_swap_buffers**: Exchanges all primary ↔ secondary fields (fb_mem, fb_phys,
  fb_dma_count, fb_dma_list, fb_resource_id, virgl_2d_resource) after each present.
- **chip_flush skips transfer** when double_buffer is active — just bumps
  activity_counter. Prevents tearing from partial front-buffer writes between swaps.
- **Graceful fallback**: If second framebuffer allocation fails, driver continues
  in single-buffer mode (deferred present).

### Fixes

- **Full-screen color flashes on menu open**: chip_virgl_fill_rect's CLEAR command
  fills the entire resource (ignores scissor). After buffer swaps, the Virgl surface
  (bound to a fixed resource at creation) could target the front (displayed) resource,
  making CLEAR visible as a bright color flash. Fixed by disabling chip_virgl_fill_rect
  entirely (returns FALSE, letting flush_all handle fills via board_mem).

### Lessons learned

- Virgl surface handles are bound to a specific resource at creation time and cannot
  be reassigned after buffer swaps. Any GPU operation through the surface may hit the
  displayed front buffer.
- BIF_GRANTDIRECTACCESS means graphics.library writes directly to board_mem without
  callbacks — dirty-rect tracking is impossible, requiring full-screen flush every cycle.

---

## v53.73 — 10.03.2026

**Phase 5d: Compositing Completion**

### Fixes

- **Double-buffer safety**: HW compositing now skips the GPU path when
  double-buffering is active. The Virgl surface is bound to a fixed resource
  at creation, so after buffer swaps it may target the displayed front buffer.
  SW composite updates board_mem; flush_all presents it next cycle.

- **Thread safety**: HW composite path now holds io_lock for the entire
  operation, preventing interleaving with flush_all's strip transfers and
  buffer swaps.

- **Source bitmap format detection**: Uses `IGraphics->GetBitMapAttr(bm,
  BMA_PIXELFORMAT)` to detect actual pixel format instead of assuming
  RGBFB_A8R8G8B8. Non-32bpp sources correctly fall back to software.

### Architecture notes

- IGraphics interface stored in ChipGPUState (opened in chip_comp_install_hook)
- HW compositing available in single-buffer mode; SW-only when double-buffered
- 13 Porter-Duff blend states, texture upload via INLINE_WRITE, DRAW_VBO rendering

---

## v53.74 — 13.04.2026

**Memory attribute hardening + GPU format optimisation + PCI_CFG fallback scaffolding**

### Changes

- **Format change**: GPU resource format switched from `B8G8R8X8_UNORM` (2) to
  `X8R8G8B8_UNORM` (4).  On PPC big-endian, P96's `A8R8G8B8` stores bytes
  `[AA, RR, GG, BB]` which matches `X8R8G8B8` layout exactly.  The 32bpp
  conversion path is now a direct word copy (no per-pixel byte shuffle).
  When `src == dst` (fallback where board_mem == fb_mem), conversion is
  skipped entirely — true zero-copy.  CLUT and 16bpp paths updated to target
  X8R8G8B8 byte order.

- **DMA buffer memory attributes**: `MEMATTRF_COHERENT` on cmd/resp/cmd3d/
  cursor DMA buffers and virtqueue rings (replaces `CACHEINHIBIT`).
  CACHEINHIBIT + COHERENT together silently drops COHERENT (kas1e's
  pa6t_eth.device discovery), so use COHERENT alone.  CPU caches stay
  active; hardware ensures DMA coherency.  Much faster than CI for small
  buffers.  New helper `chip_immu_set_coherent()`.

- **Framebuffer write-through**: `MEMATTRF_WRITETHROUGH` on fb_mem and
  fb_mem2.  L2 cache coalesces sequential pixel writes into 64-byte blocks
  while ensuring all writes are visible to DMA reads.  Pattern from
  RadeonGCN-OS4.  New helper `chip_immu_set_writethrough()`.

- **AVT_Contiguous on DMA buffers**: Prevents physical page fragmentation
  in `chip_dma_alloc()`, `VirtQueue_Allocate()`, cursor_mem, and framebuffers.
  Multi-entry DMA warning when `StartDMA` returns `n > 1`.

- **MMIO barrier discipline**: Kept `mbar` (= `sync`) for all MMIO register
  access — QEMU TCG does not flush buffered stores with `eieio` alone.
  Changed vring GetBuf barrier `lwsync` → `eieio` (vring is CI memory,
  `lwsync` doesn't order CI accesses).

- **PCI_CFG fallback scaffolding**: VIO_* macros now dispatch between
  direct MMIO (Pegasos2) and PCI config space access (spec 4.1.4.7) via
  `gs->use_pci_cfg` flag.  `chip_notify_queue()` helper centralises queue
  kick.  `vq->notify_bar_off` stored at queue setup.  PCI_COMMAND_MEMORY
  enabled early (before BAR scan) so BAR regions are live.

- **AmigaOne (-M amigaone) detection**: MMIO probe writes ACK to STATUS
  and reads back.  On failure, logs explicit "unsupported platform" error
  and returns FALSE cleanly.  PCI_CFG path NOT auto-enabled because QEMU's
  Articia S model doesn't register BAR memory regions — PCI_CFG access
  triggers `virtio_address_space_lookup` assertion and crashes QEMU.
  Code stays in tree for future use on real hardware or fixed QEMU.

### Confirmed working

- Workbench boots correctly on Pegasos2 QEMU at 1280x800 with X8R8G8B8.
  Colors correct, 32bpp path doing zero conversion.

### Known issues

- Mode switch display resize: QEMU window doesn't resize on scanout change.
  Not driver-side — likely pre-existing QEMU display backend limitation.

---

## v53.75 — 13.04.2026

**Interrupt-driven I/O + flush batching + cooperative scheduling**

### Changes

- **Clean exit on unsupported platform**: If MMIO probe fails, driver now
  returns FALSE with a clear diagnostic (platform requires `-M pegasos2`).
  Previously it would attempt PCI_CFG fallback which crashes QEMU AmigaOne.

- **Forbid/Permit yield in polling loops**: All five `for (volatile uint32
  d = 0; d < N; d++)` busy-wait spins in `chip_do_io`, `chip_do_io_locked`,
  `chip_do_io_3sg`, SET_SCANOUT retry, and cursor polling replaced with
  `IExec->Forbid(); IExec->Permit();` yield.  Scheduler can run other tasks
  and QEMU's device emulation thread gets CPU time.  Pattern from
  AmigaNVMeDevice for QEMU TCG compatibility.

- **Batched flush_all**: Full-frame conversion + one `TransferToHost` + one
  `SetScanout` + one `ResourceFlush` (instead of per-64-row strip loop).
  At 1920x1200 this reduces 19 transfers + 1 flush to 1 transfer + 1 flush.
  Double-buffer conversion no longer holds `io_lock` (no concurrent writes
  in DB mode).  Removed `FLUSH_STRIP_HEIGHT`.

- **Interrupt-driven DoIO** (new `src/chip/chip_irq.c`):
  - `chip_irq_install()` calls `MapInterrupt()` + `AddIntServer()` with
    per-chip handler.  Non-fatal — falls back to polling if install fails.
  - ISR reads ISR register (atomic ack), signals `ctrlq_wait_task` /
    `cursorq_wait_task` when ISR bit 0 is set.
  - INTx re-enabled in PCI_COMMAND after IRQ install succeeds.
  - New helper `chip_wait_ctrlq()` shared between `chip_do_io`,
    `chip_do_io_locked`, `chip_do_io_3sg`:
    - With IRQ: `AllocSignal()`, store task+mask in `gs->ctrlq_wait_task`,
      `IExec->Wait(sig_mask)` burst ×50 (~1s budget), `FreeSignal()`.
    - Without IRQ: original polling + yield path.
  - Cursor queue waiter uses `gs->cursorq_wait_task` / `cursorq_wait_mask`.

- **EVENT_IDX (deferred)**: Not beneficial for VirtIOGPU's sync-command
  flush pattern.  Documented rationale in `chip_init.c` — no submission
  pipeline to coalesce, each command blocks on its own response.  Not
  in `VIRTIO_DEVICE_FEATURES_MASK` so not negotiated.

### Confirmed working

- Clean build, all targets link.  Deployed to S:\temp\virtiogpu.chip.

### Notes

- IRQ path is guarded by `irq_installed` runtime flag; if `AddIntServer`
  fails, `irq_installed` stays FALSE and driver uses polling unchanged.
- Per-queue waiter fields (`ctrlq_wait_task`, `cursorq_wait_task`) are
  cleared on DoIO exit (normal or timeout) so stale signals never fire.

---

## v53.76 — 13.04.2026

**Quality pass: correctness, leaks, comments, diagnostics**

### Fixes

- **SetGC no-op skip** (chip_p96.c) — MUI/graphics.library can call SetGC
  repeatedly with the same mode when opening windows.  Each call was
  triggering a full-frame flush (`chip_flush_all`), producing visible
  flicker in the benchmark test.  SetGC now detects unchanged
  width/height/format and returns immediately; the flush task picks up
  any pending pixel changes on its next 5-20ms tick.  Direct fix for the
  flashing reported during gfxbench2d runs.

- **DMA buffer EndDMA pairing** (chip_init.c) — `chip_dma_alloc` now has
  a matching NULL-safe `chip_dma_free(IExec, mem, size)` that pairs every
  StartDMA with EndDMA.  The init-failure cleanup chain was updated to
  free `cursor_cmd_buf` (between resp_buf and chip_virtio_init) and
  `cmd3d_buf` (after fb_mem) which were previously leaking their DMA
  mappings on partial-init failure.  Also cleans up cursor queue vring
  in the `fail_vqs` path.  Documents driver-lifetime lock for successful
  init (chip is resident; has no Expunge).

- **Mode registration robustness** (chip_modes.c) — if all ModeInfo
  allocations for a resolution fail, the Resolution is no longer added
  to the list as a useless empty entry; it is freed and the mode is
  skipped.  NULL slots in partially-populated `res->Modes[]` remain
  supported (P96 convention for "depth not available").

- **IMMU helper hardening** (chip_init.c) — the three IMMU attribute
  helpers (`set_coherent`, `set_writethrough`, `setup_bar`) now share a
  common `chip_immu_update` implementation.  One GetInterface, one
  Forbid/SetMemoryAttrs/Permit, one DropInterface — no more fragile
  early-return + unconditional-drop patterns.  Supports both clear and
  set masks in one call (needed because W/I are mutually exclusive on
  PPC).  Behaviourally identical to the old code.

### Documentation

- **Virgl diagnostic** (chip_init.c) — when `VIRGL=NO` is negotiated,
  the log now explains that hardware compositing and 3D require
  QEMU `-device virtio-gpu-pci,virgl=on` plus a host GL stack.

- **chip_virgl_2d.c header** — new comprehensive file-level architecture
  comment: init flow, double-buffer variant, virglrenderer shader
  continuation quirk, context poisoning semantics, scanout format
  rationale, resource and handle ID namespace layout.

- **chip_gpu_3d.c header** — removed outdated "cmd3d_lock" reference
  (file uses `io_lock` only; cmd3d_lock is elsewhere).

- **Magic numbers documented**:
  - chip_board.c — bi+1630 (PCIDevice*) and bi+0x10 (BoardName) offsets
    now cite the reverse-engineering source
  - chip_modes.c — CVT-RB timing constants now cite VESA CVT v1.2 spec
  - chip_p96.c — 16-byte alignment purpose (AltiVec width / cache lines)
  - chip_composite.c — COMP_HANDLE_BASE=200 handle namespace layout
  - chip_composite.c — max_pixels_per_submit=15000 virglrenderer buffer
    cap rationale

### Diagnostic quality

- **Flush task DebugPrintF reduced**:
  - The 640x480 diagnostic pixel sample is now `#ifdef CHIP_DEBUG_VERBOSE`
  - The "flush_all(db): swap #N" line stops logging after swap #2000
    (about 30 seconds of activity).  Prevents unbounded log noise during
    long benchmark runs.

### Confirmed working

- Clean build, all targets link, deploy to S:\temp\virtiogpu.chip (v53.76).
- gfxbench2d completes successfully on v53.75 baseline (regression
  prevention is the bar for v53.76 since this is a quality pass).

### gfxbench2d baseline (v53.75, for comparison)

| Test                          | Result         |
|-------------------------------|----------------|
| copy64fx4PF (RAM→VRAM)        | 4203 MiB/s    |
| FillRect 512×512               | 395.9 MPixel/s |
| BltBitMap 512×512              | 314.0 MPixel/s |
| OverlappedBltBitMap 512×512   | 99.3 MPixel/s  |
| Random                         | 179.5 MPixel/s |
| Compositing                    | NOT SUPPORTED  |

Compositing shows as "not supported" because this QEMU run used
`-device virtio-gpu-pci` without `virgl=on`.  With Virgl enabled, the
compositing hook installs and DIPF_IS_HWCOMPOSITE is advertised.

---

## v53.77 — 13.04.2026

**SDK alignment + BoardInfo accessor refactor**

### Major changes

- **Offset → struct-member access** for BoardInfo fields.  Raw
  `(uint8*)bi + N` access in chip_board.c / chip_lib.c / chip_init.c
  replaced with `boardinfo_get_pcidevice()`, `boardinfo_set_pcidevice()`,
  `boardinfo_edid_gate()`, `boardinfo_edid_modelist()` accessors in
  p96/boardinfo.h.  Offsets and the rationale for the PCIGraphics.card
  private extended area are documented in one place rather than scattered
  through the source.

- **`BT_VIRTIOGPU` GraphicsControllerType (0x56)** — replaces the
  historical `BT_uaegfx` identifier.  VirtIOGPU is NOT UAE graphics
  emulation; diagnostic tools now see a value unique to this driver.
  rtg.library does not gate behaviour on this field so the choice is
  purely cosmetic.

- **Full V54 `BMA_*` support in chip_GetBitMapAttr**.  Previously only
  BMA_HEIGHT/WIDTH/DEPTH/BYTESPERROW were answered.  Now returns correct
  values for BMA_ISRTG, BMA_BYTESPERPIXEL, BMA_BITSPERPIXEL,
  BMA_PIXELFORMAT, BMA_ACTUALWIDTH and BMA_BASEADDRESS.  Apps using
  `IGraphics->GetBitMapAttr(bm, BMA_PIXELFORMAT)` (V54) now get a correct
  PIXF_xxx value; previously they got 0.

- **Single dispatch-point format conversion** in chip_flush.c.  The
  old if/else chain by pixel format is now a `row_conv_fn` function
  pointer selected once by `chip_pick_row_converter`, with three
  self-contained row converters (CLUT, R5G6B5, A8R8G8B8).  Adding a
  new pixel depth is a single new row function + case in the selector.

### Documentation

- **`p96/boardinfo.h` top-of-file comment** — explains the SDK boundary
  (what Hyperion publishes vs. what is reverse-engineered in this
  header), lists all SDK headers consumed, documents the PCIGraphics.card
  extended private area at the bottom of the file.

- **`chip_AllocBitMap` / `chip_FreeBitMap`** — documented the exact
  contract expected by rtg.library and V54 `AllocBitMapTagList` callers
  (raw pixel pointer returned; BitMap struct built by rtg.library
  around it; BMA_xxx queries routed through chip_GetBitMapAttr).

- **`chip_zero`** — comment expanded to explain the `-nostartfiles`
  constraint that prevents use of memset, and when to prefer
  `AVT_ClearWithValue, 0` over the explicit byte loop.

- **`pd_factors[]`** Porter-Duff table in chip_composite.c — comment
  updated to cite `enPDOperator` from `<graphics/composite.h>` and
  note that COMPOSITE_Maximum / COMPOSITE_Minimum fall through to
  the software path.

- **`chip_gpu_3d.c` file header** — updated to describe the current
  IRQ-driven wait path via `chip_wait_ctrlq`, no longer referencing
  the removed `cmd3d_lock`.

### Minor

- Board identification log line now clarifies which BoardInfo fields
  feed V54 `GetBoardDataTagList` queries (GBD_BoardName, GBD_TotalMemory,
  GBD_ChipDriver, GBD_BoardDriver).

- Mode name construction in chip_modes.c carries a clear comment
  explaining why the hand-rolled digit conversion is preferred over
  IUtility->SNPrintf at chip-init time.

### Build

- Clean build, all targets link.  No new warnings.
- Deployed as `S:\temp\virtiogpu.chip` (v53.77).

---

## v53.78 — 13.04.2026

**Signal-driven flush task with screenmode-refresh idle backstop**

### Architecture change

The flush task is now fully signal-driven instead of timer-polling.
Drawing operations and screen-switch callbacks call `chip_flush_signal_activity()`
to wake the flush task, which performs one coalesced flush and loops.
There is **no cap on flush rate** during active drawing — flushes happen
as fast as VirtIO round-trips complete (typically 1–5 kHz on QEMU,
limited by the GPU command pipeline rather than any timer in our code).

When idle (no signals), the flush task sleeps on a `timer.device` request
sized to the active screenmode's refresh interval (`refresh_us`).  This
backstop catches `BIF_GRANTDIRECTACCESS` writes from apps that bypass
our vtable, ensuring the screen still updates at one frame per refresh
even with no explicit signals.

Multitasking is preserved end-to-end:
- `IExec->Wait()` sleeps on signals — full scheduler yield, zero CPU
- `IExec->AbortIO()` cleanly cancels pending timer requests when activity
  signals fire
- No `Forbid()/Permit()` busy-waits anywhere on the flush path

### What was removed

- `gs->activity_counter` field and all `++` callsites in vtable ops
- `gs->last_seen_activity` field
- `gs->idle_cycles` field
- The hard-coded "5 ms active / 20 ms idle" adaptive interval logic
- Direct `chip_flush_all()` calls from `chip_SetSwitch`, `chip_SetGC`,
  `chip_SetPanning`, `chip_SetDisplay`, `chip_SetDPMSLevel`,
  `chip_SetColorArray`, `chip_WaitVerticalSync`,
  `chip_BlitRect`, `chip_BlitRectNoMaskComplete`, `chip_FillRect`
  Virgl path, `chip_SetSpritePosition`

### What was added

- `gs->flush_task` — task pointer published for `Signal()` from other tasks
- `gs->flush_sig_bit` / `gs->flush_sig_mask` — signal allocated by the
  flush task on entry, freed on exit
- `gs->refresh_us` — idle backstop interval, recomputed in `chip_SetGC`
  from `PixelClock / (HorTotal × VerTotal)`, clamped to 30..240 Hz
  (4166..33333 µs).  Defaults to 60 Hz (16667 µs).
- `chip_flush_signal_activity(gs)` — single-line `IExec->Signal()`
  wrapper, NULL-safe for callers that fire before the flush task starts
- `chip_compute_refresh_us()` — pixel-clock to interval conversion with
  clamping and graceful fallback

### Flush task loop

```
while (!quit):
    SendIO timer (refresh_us)
    Wait(flush_sig | timer_sig | CTRL_C)
    if timer fired:    GetMsg (drain reply)
    if signal fired:   AbortIO+WaitIO (cancel timer);  SetSignal(0, flush_sig)
    flush_all()
```

Active-drawing path: signal fires → AbortIO cancels timer → flush_all →
loop top arms a new timer → signal fires again before timer → repeat.
Effective rate = 1 flush per VirtIO round-trip.

Idle path: no signals → Wait sleeps for refresh_us → timer fires →
flush_all → loop arms next timer.  Effective rate = 1 flush per refresh.

### Build

- Clean build, all targets link.  No new warnings.
- Deployed as `S:\temp\virtiogpu.chip` (v53.78).

---

## v53.79 — 13.04.2026

**Diagnostic logging + 10-second heartbeat**

### Error logging additions

Every previously silent failure path in the GPU command layer now emits
a DCHIP line with enough context to diagnose what went wrong:

- `chip_do_io / chip_do_io_locked / chip_do_io_3sg`:
  - "control queue not initialised" when `vqs[CTRLQ]` is NULL
  - "VirtQueue_AddBuf failed rc=N" when descriptor allocation fails
  - timeouts now include cmd/data/resp sizes
- `chip_send_locked`: "short response" with actual vs required bytes
- `chip_TransferToHost2D`: "failed: resp=0xN res=N rect=(x,y wxh)"
- `chip_ResourceFlush`: same format as TransferToHost2D

These previously returned FALSE / 0 with no log line, making
post-mortem debugging difficult.

### Heartbeat

The flush task now emits a one-line heartbeat every ~10 seconds:

```
[virtiogpu.chip] HEARTBEAT: 612 flushes in 10 sec
                 (active=14 idle=598 dpms_skip=0)
                 refresh=16667us virgl=ON irq=ON dpms=0 res=1280x800
```

Fields:
- `flushes` — total `chip_flush_all()` calls in the window
- `active` — flushes triggered by an activity signal (drawing or screen switch)
- `idle` — flushes triggered by the timer backstop (vblank rate)
- `dpms_skip` — wakeups skipped because DPMS is off
- `refresh` — current backstop interval, derived from active screenmode
- `virgl` — Virgl 2D acceleration status
- `irq` — IRQ-driven I/O vs polling fallback
- `dpms` — current DPMS level (0=on)
- `res` — active resolution

Implementation uses `ITimer->ReadEClock` (no IPC, register-read fast)
to track wall-clock elapsed time.  ITimer is acquired non-fatally —
if `GetInterface` fails the heartbeat is silently disabled and the
flush task continues normally.

Final shutdown line now reports residual counters so an early exit
during hard use is also visible in the log.

### Build

- Clean build, all targets link.  No new warnings.
- Deployed as `S:\temp\virtiogpu.chip` (v53.79).

---

## v53.80 -- 13.04.2026

**Critical: fix AbortIO race in flush task. ASCII-only source.**

### Crash fix

A DSI page fault was reported in the virtiogpu.flush task while the
user was holding mouse button 1 to drag-select icons in Workbench.
The crash was a NULL pointer dereference inside an Exec list-removal
operation, called from `AbortIO()` on the timer.device IO request.

Root cause: the v53.78 flush task signal/timer race.  The flush task's
loop samples Wait() signals once, then handles them.  Between Wait()
returning and the AbortIO call, the timer can fire and the IO request
can complete -- removing itself from the kernel wait list.  Calling
AbortIO on an already-completed request causes Exec to attempt a
double-Remove, dereferencing the now-NULL ln_Succ link.

Fix: use `IExec->CheckIO()` before calling AbortIO.  If CheckIO returns
non-NULL the request is already complete and only WaitIO is needed
(WaitIO is always safe and consumes the reply message).  This is the
canonical AmigaOS pattern documented in the SDK autodocs.  Applied to
both the main loop AbortIO and the shutdown-cleanup AbortIO.

### Source character set

Stripped all non-ASCII characters from comments and code per project
preference.  Em-dash, en-dash, arrows, quotes, multiplication signs,
box-drawing, and miscellaneous Unicode replaced with ASCII equivalents
(`--`, `-`, `->`, `"`, `x`, etc.).  Source files now contain only
printable ASCII (and tabs/newlines).  No semantic changes.

### Build

- Clean build, all targets link.  No new warnings.
- Deployed as `S:\temp\virtiogpu.chip` (v53.80).

---

## v53.81 - v53.103 -- 13.04.2026

Long stabilisation series triggered by hard DSI crashes during
Workbench drag-select, and later by the gfxbench2d "grey screen"
investigation.  All symptoms converged on a handful of hot-path
choices that needed revisiting.

### Crash trail (v53.81 - v53.84)

Recurring DSI exception at kernel+0x33538, DAR=0x4, in task
`virtiogpu.flush`.  Disassembly is exec's inline Remove() helper;
r9=NULL means the node being unlinked has no `ln_Succ`.  Attempted
fixes (CheckIO guard before AbortIO; snapshotting task/mask pairs
in the IRQ handler; publish/retract ordering with Disable/Enable;
switching from raw AllocVecTags+AddTask to `IExec->CreateTaskTags`
for proper `tc_MemEntry` initialisation) did not stop the crashes --
they were palliative.

Root cause identified in v53.85: the `CheckIO`+`AbortIO` pattern
races irrecoverably with the timer.device ReplyMsg IRQ.  Dropping
AbortIO entirely and letting the backstop timer fire naturally
(with the `!timer_pending` guard at loop top preventing duplicate
SendIO) ended the crash series.

### BlitRectNoMaskComplete opcode fix (v53.86)

`chip_BlitRectNoMaskComplete` was computing `rop = opcode & 0xF`
(low nibble).  Per the graphics.library/BltBitMap autodoc the
Minterm is a full byte where the HIGH nibble holds the A=1 truth
table -- the NoMask branch is always A=1, so the high nibble is
what matters.  Every common minterm (`0xC0` SrcCopy, `0x30`
SrcInvert, `0x50` DstInvert, `0x60` SrcXorDst) collapses to 0 under
`& 0xF`, so every blit was being drawn as "fill dest with 0"
(black).  Corrected to `opcode >> 4`.

### gfxbench2d grey-screen diagnosis (v53.98 - v53.102)

After the crashes stopped, gfxbench2d still produced a grey screen
during its BltBitMap/OverlappedBltBitMap/Random phases.  Added a
hot-path trace, bitmap-pointer tracking, fill-colour ring-buffer
trace, and a force-flush diagnostic mode.  The trace proved:

- Bitmap flow was correct (`match_panning=fill_OK/blit_OK`).
- Bench drew with varied colours (not grey-on-grey); colour ring
  populated with 60k+ distinct values.
- The flush task was CPU-starved at priority 0 by the bench's
  tight per-op memory-copy loops at the same priority -- flush
  rate during OverlappedBltBitMap 512^2 dropped to 0.04 Hz (1
  flush per 25 seconds).

Fix: raised flush task priority to 10 (above user apps and
Workbench/compose, below input.device at 20).  Also moved the
CPU-heavy `chip_convert_rect` out of `io_lock`, so only the GPU
round-trips are serialised.  Flush rate during every bench phase
returned to 25-29 Hz and all tests became visible.

### Cache-attribute / barrier audit (v53.91)

Comparison against sibling VirtIO drivers (VirtualSCSIDevice,
AmigaNVMeDevice) revealed that `chip_immu_set_coherent()` was being
applied to virtqueue rings and DMA cmd/resp buffers, but the shared
`virtqueue.c` only emits `eieio` -- an I/O-ordering primitive
correct for cache-inhibited memory, not for cached-coherent.
Removed all `chip_immu_set_coherent()` calls; rings and
cmd/resp/cursor_cmd buffers now stay at default `MEMF_SHARED`
(implicit CI), matching SCSI/NVMe.  Framebuffer keeps
`MEMATTRF_WRITETHROUGH` (high-frequency CPU writes).  PCI BARs keep
`CACHEINHIBIT | GUARDED`.

### Forbid/Permit and Disable/Enable removal from GPU hot path

Audited and removed every `Forbid()`/`Permit()` pair in
`chip_wait_ctrlq`, `chip_cursor_send`, and `chip_SetScanout` retry.
Removed both `Disable()/Enable()` pairs bracketing the
`ctrlq_wait_task`/`cursorq_wait_task` publish and retract in the
IRQ-driven wait paths.  Replaced with ordered raw stores plus
`__asm__ volatile ("" ::: "memory")` compiler barriers.  Race
analysis: store order for publish is `task=cookie; mask=sig_mask`
(mid-publish IRQ sees mask=0 and short-circuits); retract is
`mask=0; task=NULL` (same short-circuit).  The only remaining
scheduler-atomic section is the once-at-init MMU attribute update
in `chip_init.c`.

### Queue drain on timeout

`chip_wait_ctrlq` now drains stale used-ring entries (up to 256)
after a timeout, preventing the cascading "TransferToHost2D failed"
pattern where one slow GPU command poisoned every subsequent one.

### Flush task creation / observability changes

- Task created via `IExec->CreateTaskTags` (primary) or
  `IDOS->CreateNewProcTags` (fallback), both at priority 10.
- `CreateTaskTags` properly initialises `tc_MemEntry` and other
  AmigaOS 4 extensions that the old manual `AllocVecTags + AddTask`
  left zero-filled.
- Hot-path observability fields added to ChipGPUState: `hot_stage`,
  `hot_iter`, `hot_wait_entries/exits`, `hot_flush_entries/exits`,
  `hot_timers_armed/fired`, `last_fill_dst`, `last_blit_src/dst`,
  `last_panning_mem`.
- `SetPanning: panning_mem X -> Y` logged only on pointer change.
- Heartbeat gained `hot stage`, `bm panning=/board=/fill_dst=`
  (match check) lines.

### Debug build gating (v53.103)

Made `DCHIP` / `DBOARD` macros gated on `-DDEBUG`.  Release build
(without the flag) compiles debug output to zero-instruction no-ops;
binary drops from ~160 KB (debug) to ~148 KB (release).  Default
Makefile build includes `-DDEBUG`.  Release build command:

```
docker run --rm -v /mnt/w/Code/amiga/antigravity:/src \
  -w /src/projects/VirtIOGPU walkero/amigagccondocker:os4-gcc11 \
  bash -c "make clean && make -j\$(nproc) all \
    CHIP_CFLAGS='-O2 -Wall -I./include \
      -fno-tree-loop-distribute-patterns -mcrt=newlib \
      -D__NOLIBBASE__ -D__NOGLOBALIFACE__'"
```

### Verified baseline (v53.103 + gfxbench2d 1280x800 A8R8G8B8)

| Phase | Flush rate | Visible? |
|---|---|---|
| Workbench idle | ~30 Hz | n/a |
| FillRect | ~29 Hz | yes, colours vary |
| BltBitMap | ~25 Hz | yes (some tests monochrome by bench design) |
| OverlappedBltBitMap 512^2 | ~27 Hz | yes |
| Random | ~27 Hz | yes, some black rects expected (bench's src bitmap is monochrome for speed-measurement purposes -- not a driver bug) |

### Post-investigation cleanup (v53.103)

Removed debug code added only to diagnose the grey-screen issue:

- `fill_color_ring[]`, `fill_color_write_idx`, `last_fill_color`,
  `last_fill_w/h`, `last_fill_format` fields from ChipGPUState.
- `FillRect[#N]: color=...` first-seen log + `FillRect[n=N]: ...`
  periodic log in `chip_FillRect`.
- Heartbeat `fill last=... ring=[...] distinct=...` line.
- Force-flush diagnostic loop (waiting on `timer_sig` only).
  Restored normal `Wait(wait_mask)` that treats `flush_sig` as a
  wake-up source again -- priority 10 solved the starvation that
  force-flush was diagnosing, and activity-signal wakeup is both
  more responsive and more efficient than unconditional timer
  ticks.

Kept general-purpose observability (hot_stage/counters, bitmap
tracking, SetPanning-change log, `wait_ctrlq` TIMEOUT log with
drain count) -- lightweight and useful for future debugging.

### Build

- Clean build.  No new warnings.
- Deployed as `S:\temp\virtiogpu.chip` (v53.103, release build ~148 KB).

---

## v53.104 -- 26.04.2026

**P96 SDK / RadeonRX disassembly cleanup pass.**

Triggered by an external RTG-driver review (zz9000-drivers, then the
canonical Picasso96 P96CardDevelop SDK, then a binutils disassembly of
PCIGraphics.card v53.18 and RadeonRX.chip.debug).  Goal was to identify
gaps and bugs in our chip driver based on what canonical / sibling
drivers actually do.

### Functional additions

- **`chip_GetVSyncState`** -- replaces the `stub_bool` slot.  Synthetic
  vblank phase: a new `volatile UBYTE vsync_phase` field in
  `ChipGPUState` is toggled by the flush task on every wakeup
  (signal-driven or timer backstop).  Apps polling this in a
  `while(GetVSyncState()) ; while(!GetVSyncState()) ;` loop now see a
  level change at least once per refresh interval.

- **`chip_BlitPlanar2Direct`** -- new vtable entry at canonical slot 41.
  Same plane-scan logic as `chip_BlitPlanar2Chunky`; each computed
  palette index is mapped through `clut->Colors[idx] & clut->ColorMask`
  to a direct ARGB color, written at the destination's native depth
  (32bpp = direct store, 16bpp = R5G6B5 pack, 8bpp = pass-through
  index).  Handles minterm `0xC0` (src copy) only -- other minterms
  return without touching the destination so rtg.library's
  `BlitPlanar2DirectDefault` produces the result.  Used by P96 when an
  app draws a classic 8-bit palette BitMap onto a TrueColor RTG screen.
  Slot 41 confirmed against `P96CardDevelop/PrivateInclude/boardinfo.h`.

- **New `struct ColorIndexMapping`** in `boardinfo.h` matching the
  canonical layout (`ColorMask` + `Colors[256]`).

### Flag corrections

- **`BIF_BLITTER`** added to `bi->Flags`.  P96 docs require it whenever
  any 2D acceleration is implemented; we have FillRect/BlitRect via
  Virgl when available and CPU loops otherwise -- both qualify.

- **`BIF_HASSPRITEBUFFER`** added.  VirtIO cursor plane is a permanent
  64x64 ARGB GPU resource independent of the framebuffer; rtg.library
  no longer needs to allocate a `MouseSaveBuffer` it would never use.

### Bit-number corrections in `boardinfo.h` (latent bugs, no behaviour change)

The previous reverse-engineering had two `BIB_*` constants at wrong
bit positions that no shipping code happened to set.  Disassembly of
RadeonRX.chip's flag-update sequence (`ori r10,r10,16; stw r10,186(r31)`
at 0x1001b70) confirms canonical bit numbering is what AOS4 uses:

| Constant | Was | Now (canonical, verified) |
|---|---|---|
| `BIB_VBLANKINTERRUPT` | 25 | **4** |
| `BIB_CACHEMODECHANGE` | 27 | **3** |

Header now also has `BIB_HASSPRITEBUFFER=5`, `BIB_VGASCREENSPLIT=6`,
`BIB_DBLCLOCKHALFSPRITEX=7`, `BIB_SYSTEM2SCREENBLITS=25`,
`BIB_PALETTESWITCH=27`, `BIB_DACSWITCH=28`, `BIB_NOMASKEDBLITS=29`,
`BIB_OVERCLOCK=31` at canonical positions.

### Identification field corrections (real bug)

`bi->BoardType` was never set (defaulted to 0 = `BT_NoBoard`).
`bi->GraphicsControllerType` had `BT_VIRTIOGPU` (a `BTYPE` value) in
the `GCTYPE` slot.  `bi->PaletteChipType = 14` = canonical
`PCT_TIPermedia2`, falsely advertising a Permedia2 DAC.  Now:

```
bi->BoardType              = BT_VIRTIOGPU;  /* @170 BTYPE */
bi->PaletteChipType        = 0;             /* @174 PCT_Unknown */
bi->GraphicsControllerType = 0;             /* @178 GCT_Unknown */
```

`bi->VBIName` (char[32] at +20) populated with `"VirtIOGPU.vbi"` --
was previously zero-filled, making it invisible in tools that walk
`IExec->IntServer` chains.

### Extension-area accessors (offsets verified by disassembly)

Both `PCIGraphics.card` and `RadeonRX.chip.debug` were disassembled
with `powerpc-linux-gnu-objdump`.  RadeonRX preserves the canonical
P96 layout for the @1546-1638 region (memory-space + sync block +
`MaxPlanarMemory`); only the smaller @1620-1634 window is reorganised
in AOS4 (where `PCIDevice*`, EDID gate, EDID modelist live).

New accessor inlines in `boardinfo.h`:

| Inline | Offset | Notes |
|---|---|---|
| `boardinfo_set_memory_space(bi, base, size)` | +1546 / +1550 | mirrors `MemoryBase`/`MemorySize` to canonical `MemorySpaceBase/Size`.  RadeonRX writes the same in its InitChip. |
| `boardinfo_set_sync_period(bi, us)` | +1566 | canonical `SyncPeriod` (frame interval in microseconds).  Seeded to 16667 (60 Hz) at init; updated by `chip_SetGC` whenever the active mode's `refresh_us` changes. |
| `boardinfo_get_sync_period(bi)` | +1566 | read-back companion. |

`chip_init.c` now calls these alongside the existing `MemoryBase/Size`
fills.

### Documentation: every `_fp*` slot now annotated

The nine reserved slots in our `BoardInfo` struct (`_fp21..24`,
`_fp43..52`, `_fp57`, `_fp65..67`) carry comments naming the canonical
function (`ScrollPlanar/Default`, `UpdatePlanar/Default`,
`EnableSoftSprite/Default`, `AllocCardMemAbs`, `SetSplitPosition`,
`ReInitMemory`, `GetCompatibleDACFormats`, `CoerceMode`,
`Reserved3Default`, `WriteYUVRect/Default`, `GetFeatureAttrs`,
`CreateFeature/SetFeatureAttrs/DeleteFeature`).  Documentation only --
slot signatures stay opaque `void(*)(void)` because AOS4 has not been
verified to match canonical for these.

### Deferred (still blocked on reverse engineering)

- **`BIF_VBLANKINTERRUPT` + `WaitQ` walk**.  Bit number now correct (4)
  and `WaitQ` is most likely at canonical +1604, but the *node format*
  in `WaitQ` is set by rtg.library, not by canonical Picasso96.  Walking
  it blindly risks signalling the wrong tasks or interleaving with
  rtg.library's own driver.  Needs reverse engineering of rtg.library's
  WaitTOF queue convention.

- **`MonitorWidth`/`Height` from EDID at canonical +2434/+2436**.
  RadeonRX uses +2018-2738 heavily for chip-private fields, well inside
  canonical's `SecondaryCLUT[256]` range (+1650-2418).  AOS4 has
  reorganised the area beyond +1650; canonical +2434/+2436 cannot be
  trusted as `MonitorWidth/Height` without independent confirmation.
  No `Monitor` strings in either driver's debug output, so no live
  consumer to validate against either.

### Reference material captured

- **P96CardDevelop SDK** (Picasso96Develop/PrivateInclude/boardinfo.h
  + settings.h + HardWare/CirrusGD5434.chip.asm) -- canonical 68-slot
  vtable order, full BoardInfo struct, BIB_* bit numbers, ABMA_* /
  GBMA_* / FA_* tag values.  This is the chip-driver-private SDK that
  AOS4 PCIGraphics.card was forked from.

- **AOS4 RadeonRX.chip.debug** (Enhancer Software 2.2) -- 750KB ELF,
  not stripped of code but stripped of symbols.  Useful as a reference
  for AOS4-specific `BoardInfo` field offsets.

### Build

- Clean build, no new warnings.  `S:\temp\virtiogpu.chip` (v53.104,
  ~160KB debug build).  No regression to gfxbench2d baseline.

---

## v53.105 -- 26.04.2026

**Second sweep on P96 SDK / RadeonRX findings.**  Caught a real bug
in v53.104 and added four small canonical-region improvements.

### Bug fix: SyncPeriod write was clobbering PCI Vendor/Device IDs

v53.104 wrote `SyncPeriod` (ULONG) at canonical offset +1566 in both
`chip_InitCard_C` (seed) and `chip_SetGC` (mode change update).  A
deeper PCIGraphics.card disassembly revealed the same bytes are used
by AOS4 PCIGraphics.card to stash the PCI Vendor (UWORD at +1566) and
Device (UWORD at +1568) IDs:

```
0x1001120: IPCI->ReadConfigWord(self, 0) -> sth r3,1566(r28)   ; Vendor
0x1001134: IPCI->ReadConfigWord(self, 2) -> sth r3,1568(r28)   ; Device
```

PCIGraphics.card never reads them back (only stores) -- by elimination
they are read by graphics.library v54 GBD_PCIVendorID / GBD_PCIProductID
queries.  Our SyncPeriod ULONG write at +1566 was overwriting both
UWORDs with `0x0000:0x411B` (= 16667 split as high:low UWORDs in PPC
big-endian), making the chip appear to have vendor 0x0000.

Fix: removed the `boardinfo_set_sync_period` calls.  `refresh_us` stays
internal to ChipGPUState, where it always was.  RadeonRX.chip has the
same bug in its InitChip (writes a ULONG at +1566) -- this is a real
RadeonRX bug, not a counter-example.

### New extension-area writes (offsets verified, slot purposes confirmed)

- **PCI Vendor/Device ID at +1566 / +1568** -- new
  `boardinfo_set_pci_id(bi, 0x1AF4, 0x1050)` accessor; called from
  `chip_InitCard_C`.  Without this our SetMethod-injected device
  reports vendor:device 0/0 to V54 GBD queries.

- **SyncTime at +1558 / +1562** -- zeroed via `boardinfo_set_sync_time(bi, 0, 0)`.
  Could be set from `IExec->CurrentTime()` for a meaningful pseudo-vblank
  baseline, but timer.device isn't open at chip_InitCard time.  Zero is
  explicit "unset" rather than uninitialised stack data.

- **EssentialFormats at +1616** -- `boardinfo_set_essential_formats(bi, bi->RGBFormats)`.
  Mirrors the chip's advertised format mask so apps querying without
  "all" see the same values.

- **MaxBMWidth / MaxBMHeight at +1642 / +1646** -- 4096 each, matching
  our `MaxHorValue` / `MaxVerValue` arrays.  Used by `p96AllocBitMap`
  to clip oversize bitmap requests.

### Real `chip_SetInterrupt` (replaces `stub_bool`)

Now latches the requested enable/disable state in
`g_chip_state->vbi_enabled`.  No actual VBlank IRQ generation -- VirtIO
GPU has no vblank source we can drive without rtg.library WaitTOF queue
cooperation -- but storing the request is more correct than the bool
stub and lets future work gate on the flag.

### Build

- Clean build, no new warnings.  `S:\temp\virtiogpu.chip` (v53.105).

---

## v53.106 -- 26.04.2026

**graphics.library.kmod deep-dive: VBlank IRQ unblocked, AOS4
extension layout sharper, two more regressions caught.**

### graphics.library.kmod added to the reference set

`graphics.library.kmod` (1.1 MB, ~225K lines disassembled) and
`RadeonHD.chip.debug` (1.9 MB) were added alongside the existing
`PCIGraphics.card` / `RadeonRX.chip.debug` reference binaries.
Strings, function offsets, and field-access patterns were extracted
to answer the v53.105 deferred items.

### Item unblocked: BIF_VBLANKINTERRUPT + WaitQ drain

Canonical `PiccoloSD64.card.asm` (in `P96CardDevelop/HardWare/`)
shows the chip-driver pattern:
- chip's HardInterrupt handler clears the chip-level vblank flag,
- then calls `IExec->Cause(&bi->SoftInterrupt)`.
- rtg.library installs the SoftInterrupt; its handler walks
  `bi->WaitQ` and signals every task waiting on `WaitTOF` /
  `WaitBOVP`.

**Key insight**: the chip never touches `WaitQ` itself.  The node
format is rtg.library-private; chip drivers don't need it.  Adapted
for VirtIO GPU (no real vblank IRQ source): drive `Cause()` from
the existing flush-task refresh tick.  `Cause()` is callable from
any task context, and the flush task already wakes once per refresh
interval.

Implementation:
- `BIF_VBLANKINTERRUPT` added to `bi->Flags`.
- `chip_SetInterrupt` (already real in v53.105) latches state in
  `gs->vbi_enabled`.
- Flush task fires `IExec->Cause(&bi->SoftInterrupt)` on each
  wakeup when `vbi_enabled && bi`.  ~5 lines added in `chip_flush.c`
  next to the existing `vsync_phase` toggle.

### Item still deferred: MonitorWidth/Height

graphics.library.kmod was scanned for any UWORD or ULONG store at
candidate offsets.  Only UWORD writes in extension area: `bi+1382..1404`
(canonical Mouse fields) and `bi+1624` (lone signed UWORD, accessed
via `lha`/`sth` -- consistent with canonical `YSplit` repositioned
to AOS4-specific offset).  ULONG writes in `1500..2500` range:
`1554, 1558, 1562, 1616, 1620, 1626, 1634, 1638, 1642, 1646, 1710,
1726, 1778, 1782` -- none are paired-UWORD writes that look like
mm dimensions, and the canonical positions @2434/@2436 are completely
unaccessed.  Conclusion: AOS4 does not store MonitorWidth/Height in
`BoardInfo` (or stores them at AOS4-specific offsets that have no
observable consumer in graphics.library or any chip driver we can
inspect).  Stays deferred.

### Item still deferred: rename `_fp43..52` / `_fp65..67` slots

graphics.library.kmod write patterns into vtable slots:

| Slot | Offset | graphics.library installs default? |
|---|---|---|
| 41 BlitPlanar2Direct | bi+438 | YES (`stw`) |
| 42 BlitPlanar2DirectFB | bi+442 | YES |
| **43..50** | bi+446..474 | **NO** -- no `stw` writes observed |
| 51 WriteYUVRect | bi+478 | YES |
| 52 WriteYUVRectFB | bi+482 | YES |
| 53..56 GetVSyncState..ResetChip | bi+486..498 | YES |
| 57 GetFeatureAttrs | bi+502 | YES |
| 58..64 AllocBitMap..SetSpriteColor | bi+506..530 | YES |
| 65..67 CreateFeature..DeleteFeature | bi+534..542 | YES |

Only slots 43..50 (canonical EnableSoftSprite[Default],
AllocCardMemAbs, SetSplitPosition, ReInitMemory,
GetCompatibleDACFormats, CoerceMode, Reserved3Default) have NO
graphics.library default install.  Either AOS4 doesn't use them at
all, or it only uses them when chips advertise specific flags (e.g.
`BIF_VGASCREENSPLIT` for SetSplitPosition) -- we can't tell from
disassembly alone.  Annotation comment in `boardinfo.h` updated to
reflect the slot-by-slot evidence.  Slots 65..67 confirmed canonical
on AOS4; could be wired to typed pointers if implementations existed.
None do for VirtIOGPU.

### Regression caught: bi+1642 / bi+1646 are NOT MaxBMWidth/Height

v53.105 added `boardinfo_set_max_bm_size(bi, 4096, 4096)` writing to
`bi+1642 / bi+1646` based on canonical layout.  Disassembly of
`graphics.library.kmod 0x104db5c` shows the classic insert-at-head
idiom on bi+1646:

```
lwz r10, 1646(r9)        ; current head
stw r10, 18(r30)         ; new->next = head
stw r30, 1646(r9)        ; head = new
```

`bi+1646` is a hash-chain head (BitMapExtra lookups), not
`MaxBMHeight`.  `bi+1642` is similarly read as a pointer in mouse
paths, not a dimension.  Both `boardinfo_set_max_bm_size` and
`boardinfo_set_essential_formats` calls were removed from
`chip_init.c`.  The accessor inlines were renamed
`*__UNSAFE_AOS4` to discourage re-adoption.

The driver still works because graphics.library reinitialises +1646
on its first use after our chip InitCard returns -- our 4096 was
overwritten before being dereferenced -- but it is a real corruption
that was waiting to bite under different timing.

### Build

- Clean build, no new warnings.  `S:\temp\virtiogpu.chip` (v53.106).

### Net evidence summary added to `aos4_boardinfo_offsets.md`

- VBI: HardInterrupt -> `Cause(SoftInterrupt)` -> rtg.library walks WaitQ.
- Slots 43..50: graphics.library installs no defaults; wiring unsafe.
- Slots 51/52, 65..67: confirmed function pointers via stw writes.
- bi+1642/+1646: list/pointer state, not dimensions.
- bi+1616: OR-accumulator, exact semantics still ambiguous.
- bi+1624: probably AOS4 YSplit (signed UWORD).
- bi+1554, +1626, +1710, +1726, +1778, +1782: ULONG fields written by
  graphics.library, semantics unidentified.

---

## v53.107 -- 26.04.2026

**2D-scope improvements: blob framebuffer, hotplug mode list, real
WaitVerticalSync / GetCurrentY, first-fit allocator with coalescing.**

Four items from the post-RE deferred list landed; one (dirty-rect
flush) stays deferred because it can't be done safely under
BIF_GRANTDIRECTACCESS without per-page dirty tracking.

### Item 1 -- VIRTIO_GPU_F_RESOURCE_BLOB framebuffer

`chip_ResourceCreateBlob` extended to accept a DMA backing list (was
header-only).  When the host advertises VIRTIO_GPU_F_RESOURCE_BLOB,
chip_init now creates the framebuffer as `BLOB_MEM_GUEST` instead of
`CREATE_2D + ATTACH_BACKING`.  The host imports our DMA pages directly
and reads them on every RESOURCE_FLUSH, so we no longer need a
TRANSFER_TO_HOST_2D round-trip per frame -- saves ~4 MB of MMIO+DMA
traffic at 1280x800 32bpp / 60 Hz, the dominant cost at high
resolutions.

`chip_TransferToHost2D` short-circuits to TRUE when the target is the
blob framebuffer, so existing call sites (chip_flush, chip_flush_all,
init's first flush) Just Work without per-site changes.  Falls back
to the classic 2D path on any blob-create failure (older QEMU that
advertises the feature but doesn't fully support GUEST blobs).

`gs->fb_is_blob` records which path won; logged once at init.

### Item 3 -- Hotplug mode list refresh

`chip_register_modes`'s inline Resolution + ModeInfo allocation block
extracted into a reusable `chip_add_mode(gs, w, h, ..., slot_idx)`
helper.  CVT-RB timing is synthesised when the caller passes 0 for
hor_total / ver_total, matching the original EDID-derived path.

`chip_handle_display_change` -- previously logged the new size and
exited -- now calls `chip_add_mode` with the host's new dimensions.
Already-registered resolutions are detected and skipped.  New entries
get DisplayIDs in the safe `0x0005xxxx` range, encoded with a
hot-plug-specific bias (`0xC0 + count`) so they can't collide with
the static-table indices.  ScreenMode prefs picks them up without a
reboot.

We deliberately do NOT auto-resize the active scanout to match the
host window -- that creates a feedback loop (SET_SCANOUT triggers
QEMU window resize triggers new event triggers SET_SCANOUT...).
The user picks the new mode through normal Workbench UI.

### Item 4 -- Real WaitVerticalSync + GetCurrentY

`chip_WaitVerticalSync` (vtable slot 18) was a no-op signal-the-flush-
task wrapper that returned instantly.  Now it allocates a per-call
signal, publishes itself in `gs->wait_vsync_task` / `wait_vsync_mask`
under Forbid/Permit, and IExec->Wait()s.  The flush task's refresh
wake-up loop notices the registration, atomically clears it, and
Signal()s the waiter.  Single-slot rendezvous; if a second waiter
arrives while one is parked, the second replaces and races for the
same wake-up -- both wake within one refresh interval.

`chip_GetCurrentY` (vtable slot 54) was `stub_ulong` returning 0.
Now synthesises a beam Y position from EClock delta:

    delta_us  = (now_eclock - last_vsync_eclock) * 1e6 / eclock_freq
    fraction  = delta_us / refresh_us            (clamped to [0..1])
    y         = fraction * active_height

The flush task acquires ITimer at startup, publishes
`gs->ITimer / gs->eclock_freq / gs->last_vsync_eclock_lo`.  The last
field is updated on every refresh wake-up, so GetCurrentY callers
running in other tasks just need a fast `IExec->ReadEClock` register
read (no IPC).  ITimer is retracted (NULL stored back, compiler
barrier) before being dropped on flush-task exit.

### Item 5 -- First-fit + adjacent-free coalescing allocator

Old `board_alloc/board_free` did top-compaction only -- a free in
the middle of the heap was permanent dead space until everything
above it was also freed.  ScreenMode prefs cycling resolutions
typically allocates a new bitmap and frees the old one in LIFO
order, which mostly worked, but a stuck allocation in the middle
ended the heap.

New allocator:

  * Slot table now models holes explicitly (used=FALSE entries).
  * `board_free` marks the slot free and runs `board_coalesce_at`
    which merges with the immediately-prior and immediately-next
    free slots, then drops trailing free slots (top compaction
    is preserved as a special case).
  * `board_alloc` first-fits through the slot table looking for
    any free entry of sufficient size; splits a larger hole when
    it finds one, or extends at the top if no hole fits.

Same 64-slot table, same 16-byte alignment, same out-of-memory
return path.  Recovers fragmentation that the old top-only code
left stranded.

### Item 2 -- Dirty-rect flush: still deferred

Implementation considered: aggregate a per-frame dirty rect from
chip_flush calls, transfer only that area in the flush task.

Why not landed: BIF_GRANTDIRECTACCESS apps write to board_mem
without calling chip_flush -- the dirty rect would be stale and
direct-access pixels would never be transferred.  Without per-page
dirty tracking (which a virtual GPU has no API for) we can't
distinguish "vtable-only workload" (safe to partial-transfer) from
"vtable + direct-access workload" (must full-transfer).

A useful subset would gate the optimisation behind an opt-in env
var for vtable-heavy workloads (Workbench idle, MUI).  Left as a
follow-up.

### Build

- Clean build, no new warnings.  `S:\temp\virtiogpu.chip` (v53.107).

---

## v53.108 -- 26.04.2026

**Quality + diagnostics: SetSwitch parking, init-leak audit,
multi-DTD EDID parser, heartbeat fade-out, drawing-mask init.**

### Real `chip_SetSwitch(FALSE)` -- park RTG output

Previously logged the request and signalled the flush task as if
nothing changed.  Now latches `gs->rtg_output_enabled` (TRUE/FALSE)
and the flush task gates on it alongside the existing DPMS check:
when output is parked, refresh-tick wake-ups skip the host transfer
and increment the same `hb_dpms_skip` counter for visibility.
SetSwitch(TRUE) lets normal flushing resume on the next tick.

The skip path applies to the flush-task driven full-frame work; per-
rect chip_flush calls from vtable ops still update GPU resources for
correctness in case SetSwitch(FALSE) was a brief hand-off.  Match
canonical Picasso96 contract: chip is allowed to redirect resources
internally but mustn't drive the host scanout while parked.

### Init leak audit -- second framebuffer cleanup chain

`fb_mem2 / fb_dma_list2` (the optional double-buffer back-buffer) had
a complete inline unwind for *its own* allocation failure but no
cleanup in the late `goto fail_*` paths.  Any failure between
"fb_mem2 alloc succeeded" and the end of `chip_InitCard_C` would
leak the buffer + its DMA mapping.  Fixed: `fail_fb_dma_list:` now
NULL-safely `EndDMA` + `FreeVec` + `FreeSysObject(ASOT_DMAENTRY)`
the second framebuffer ahead of the existing fb_mem cleanup.

The chip is RTF_COLDSTART resident so the leak only ever bit on a
hard chip-init failure (never observed in normal boot), but proper
unwind matches AmigaOS SDK convention and prevents future regressions.

### Multi-DTD EDID parser

Previously we extracted only the first Detailed Timing Descriptor
(bytes 54-71 of the base block).  Real EDID 1.3+ blobs carry up to
4 DTDs in the base block (offsets 54, 72, 90, 108) and CEA-861
extension blocks add up to 6 more per extension.  The new parser
walks all of them, deduplicates (W, H) pairs, and stores up to 16
distinct timings in `gs->edid_dtd_w[]/h[]`.

`chip_register_modes` now feeds every DTD entry into `chip_add_mode`,
so apps see all monitor-advertised resolutions (not just the
preferred one) in ScreenMode prefs.  `chip_add_mode`'s built-in
deduplication against `bi->ResolutionsList` keeps the union clean
when EDID DTDs overlap with the static mode table.

### Heartbeat fade-out

Heartbeat logs at once per 10 seconds for the first hour
(`gs->hb_count_total < 360`), then fades to once per minute.
Long-running session logs no longer get dominated by routine
heartbeats while still giving a regular liveness pulse.  Counter
preserved across heartbeats; reset only on driver reload.

### `bi->Mask` and `bi->Border` init

`bi->Mask` left at the allocation default of 0 means "no bits
valid" -- can confuse `SetWriteMask`-aware paths.  Now explicitly
initialised to `0xFFFFFFFF` ("all bits valid") in `chip_InitCard_C`,
matching what 32bpp callers expect.  `bi->Border` explicitly set
to `FALSE` for clarity (was implicitly 0 from PCIGraphics.card's
allocation).

### Skipped from this batch

- **`ResetChip` (slot 56)**: canonical reference drivers
  (CirrusGD5434.chip, PiccoloSD64.card) leave it un-wired.  Keeping
  `stub_void`.
- **ENV/Tooltype-driven runtime tuning**: chip init runs at
  RTF_COLDSTART pri 65, before dos.library is open, so `IDOS->GetVar`
  isn't available there.  Could be wired post-flush-task-start but
  the runtime knobs (no-doublebuffer, no-virgl, debug-level) already
  baked into init are too late to retract.  Deferred until a
  meaningful runtime-tunable surface emerges.
- **Multi-scanout**: VirtIO advertises `num_scanouts` and we already
  populate `gs->display_info[]` for each.  Exposing them as separate
  PCIGraphics.card boards needs API work we don't yet understand
  (PCIGraphics.card's BoardInfo allocator is private).  Deferred.
- **Mode-resize without full shutdown**: `chip_SetGC` already avoids
  re-creating the GPU resource on no-op (same w/h/format).  A real
  resize-in-place needs SET_SCANOUT-with-new-size ordering work
  that touches the double-buffer state.  Deferred.

### Build

- Clean build, no new warnings.  `S:\temp\virtiogpu.chip` (v53.108).

---

## v53.109 -- 26.04.2026

**Forbid/Permit -> AOS4 mutex migration.**

Per https://wiki.amigaos.net/wiki/Exec_Mutexes, mutexes have lower
system-wide overhead than Forbid/Permit because only contending tasks
block, not the whole system.  Audit:

| Site | Was | Now |
|---|---|---|
| `chip_p96.c` `chip_WaitVerticalSync` -- publish/retract rendezvous (3 pairs) | Forbid/Permit | `MutexObtain`/`MutexRelease` on new `gs->vsync_mutex` |
| `chip_flush.c` flush task -- snapshot/clear rendezvous | Forbid/Permit | Same mutex |
| `chip_p96.c` `chip_WaitVerticalSync` -- "yield once" fallback on AllocSignal failure | Forbid/Permit (broken: doesn't yield) | Removed; just return |
| `chip_init.c` `chip_immu_update` -- MMU page-attr read-modify-write | Forbid/Permit | **Kept** -- mutex wouldn't help (no other code uses it) and we genuinely need scheduler atomicity for the get/set sequence.  Documented in code. |

The "yield" Forbid/Permit removal is a real bug fix: `Forbid();
Permit();` doesn't yield the CPU on AmigaOS -- it brackets a critical
section but contains no schedule point.  The previous code thought it
was yielding when AllocSignal failed; in practice it was a no-op
followed by an immediate return.  The mutex-based path now handles
the AllocSignal-failure case as "no vsync hardware available",
matching canonical chip-driver behaviour.

### New ChipGPUState field

`APTR vsync_mutex` -- non-recursive `ASOT_MUTEX` allocated in
`chip_InitCard_C`, freed via the existing `fail_fb_dma_list:` cleanup
chain (NULL-safe, won't be held across the failure since the chip
vtable is never published in that path).

### Rationale for keeping `ObtainSemaphore`/`ReleaseSemaphore`

Roughly 55 call sites use `struct SignalSemaphore` + `Init/Obtain/
ReleaseSemaphore` -- mostly `gs->io_lock` around VirtIO command-queue
transactions.  These are a legitimate AOS pattern and the migration
risk + scope to mutexes is large.  Left alone.

### Rationale for keeping the MMU `Forbid/Permit`

A mutex can only protect against contenders that take the same mutex.
`SetMemoryAttrs` is a system-wide operation -- if any other task
modifies the same page's attributes, we need them to also use *our*
mutex for protection to apply.  No other code does, because no other
code knows about `gs->vsync_mutex` or any putative MMU mutex.
`Forbid/Permit` here serves the actual need: prevent task switching
for the brief read-modify-write window so the get/set pair is atomic
from this task's perspective.  Held for << 1 us.  Comment in the code
explains the choice so future maintainers don't "fix" it.

### Build

- Clean build, no new warnings.  `S:\temp\virtiogpu.chip` (v53.109).

---

## v53.110 -- 26.04.2026

**SDK convention sweep: SignalSemaphore -> ASOT_MUTEX migration,
list traversal modernisation.**

Audit against the AmigaOS 4 SDK wiki pages (`Exec_Tasks`,
`Exec_Signals`, `Exec_Lists_and_Queues`, `Exec_Messages_and_Ports`,
`Exec_Semaphores`, `Exec_Mutexes`) found the codebase mostly canonical
on tasks/signals/ports.  Two areas needed updates:

### io_lock / cursor_lock / cmd3d_lock -> ASOT_MUTEX

All three semaphores were embedded `struct SignalSemaphore` initialised
via `IExec->InitSemaphore`.  None used `ObtainSemaphoreShared` -- every
acquire is a write-exclusive section over cmd_buf/resp_buf or the
virtqueue rings.

Per the SDK wiki: *"Choose Mutexes when available because they will
not use Forbid/Permit locking internally so the overall system will
be less affected.  Mutexes also support optional recursion (nesting)."*

Migrated:

- `chip_state.h`: `struct SignalSemaphore io_lock` -> `APTR io_lock`
  (similar for cursor_lock and cmd3d_lock).
- `chip_init.c`: `InitSemaphore(&gs->io_lock)` -> `AllocSysObjectTags(
  ASOT_MUTEX, ASOMUTEX_Recursive, FALSE, TAG_END)` for each of the
  three locks.  Allocation failure of io_lock or cursor_lock is fatal
  (we can't run safely without serialisation); cmd3d_lock failure
  disables Virgl gracefully.
- New `fail_gs:` cleanup releases all three mutexes NULL-safely
  (`FreeSysObject(ASOT_MUTEX, ...)`); cmd3d_lock cleanup also moved
  here from `fail_fb_dma_list:` so every late failure path benefits.
- ~60 call sites: `IExec->ObtainSemaphore(&gs->io_lock)` ->
  `IExec->MutexObtain(gs->io_lock)`, similarly for Release and the
  other two locks.  Mechanical sed-driven rewrite, no logic changes.

The earlier ad-hoc `vsync_mutex` (added in v53.109) now matches the
project-wide pattern.

### List traversal in chip_modes.c

Two raw `mln_Succ` pointer walks rewritten to use `IExec->GetHead` /
`IExec->GetSucc`, per `Exec_Lists_and_Queues` SDK convention.  The
empty-list count loop also gained an explicit `IsMinListEmpty()`
fast-path guard.  Equivalent behaviour, more self-documenting code:

```c
for (node = IExec->GetHead((struct List *)&bi->ResolutionsList);
     node != NULL;
     node = IExec->GetSucc(node)) {
    ...
}
```

### Audit conclusions (no changes needed)

- **Tasks**: `IExec->CreateTaskTags` is the primary task-creation path,
  with `IDOS->CreateNewProcTags` fallback for the DOS-needs case.
  Stack 32 KB exceeds the 16 KB minimum.  `tc_MemEntry` properly
  initialised by CreateTaskTags.  Canonical.
- **Signals**: Per-task ownership respected -- flush task allocates
  its own signal, `chip_WaitVerticalSync` allocates per-call in the
  calling task.  Cross-task `IExec->Signal(task, mask)` used
  correctly.  `SIGBREAKF_CTRL_C` honoured in flush task.  Canonical.
- **Ports/Messages**: `AllocSysObjectTags(ASOT_PORT, ...)` already in
  use (chip_flush.c).  `GetMsg` drains timer replies.  No legacy
  `CreatePort` calls.  Canonical.
- **Read-shared semaphore mode**: deliberately not introduced.  Every
  protected section in our driver mutates shared state (cmd_buf,
  resp_buf, virtqueue rings).  Shared mode would have no callers.

### Build

- Clean build, no new warnings.  `S:\temp\virtiogpu.chip` (v53.110).

---

## v53.111 -- 26.04.2026

**Concurrency hardening + dedup + doc refresh.**

### alloc_mutex -- protect the bitmap allocator

`board_alloc` / `board_free` in `chip_p96.c` mutate file-scope statics
(`board_allocs[]`, `num_board_allocs`) from any task that opens or
closes an RTG bitmap.  rtg.library typically holds `bi->BoardLock`
across vtable callbacks, but that's an external invariant we don't
own.  Two simultaneous AllocBitMaps could corrupt the slot table or
double-allocate.

Fix: new `gs->alloc_mutex` (ASOT_MUTEX, non-recursive) wraps the
read-modify-write in both helpers.  NULL-safe (init path is single-
threaded so locking isn't needed before chip_InitCard_C completes).
Allocated next to io_lock/cursor_lock; freed in fail_gs cleanup.
~25 lines.

### chip_do_io / chip_gpu_send dedup

`chip_gpu_3d.c` carried a verbatim copy of the control-queue
transaction helpers from `chip_gpu_cmds.c` (commented "could be
refactored into a shared helper later").  Promoted the cmds.c
versions to public, added prototypes to `chip_state.h`, removed
the 3d.c duplicates, sed-renamed call sites.  Hottest GPU code
path now has a single source of truth -- no more bug-fix drift
risk.  ~80 lines removed, no behaviour change.

`chip_do_io_3sg` (3-SG variant for SUBMIT_3D) stays in chip_gpu_3d.c
since it has no analogue in cmds.c.

### Documentation refresh

`CLAUDE.md` "Source Tree" section rebuilt from scratch:

- Removed stale references to the deleted `virtiogpu.device` path
  (`src/device.c`, `src/Init.c`, etc.).
- Added missing files: `chip_irq.c`, `chip_composite.c`, `chip_modes.c`,
  `aos4_boardinfo_offsets.md`, `PHASE5B_COMPOSITING_PLAN.md`,
  `virtiov1.2-gpu.txt`, `minigl/`, `tools/test_composite.c`.
- Each entry now has a one-line description matching its current role
  (e.g. chip_p96.c notes its size + future-split candidate).
- Migration notes block at the bottom highlights what changed since
  v53.103.

### Build

- Clean build, no new warnings.  `S:\temp\virtiogpu.chip` (v53.111).

---

## v53.112 -- 26.04.2026

**Doc cleanup, chip_p96.c split, mode-resize back-buffer fix,
virtiogpu_info Shell utility.**

### Source tree split: chip_alloc.c + chip_blit.c

`chip_p96.c` (~1700 lines) split three ways:

- `chip_alloc.c` (~290 lines) -- board memory allocator (board_alloc /
  board_free with first-fit + coalesce + alloc_mutex protection),
  chip_AllocBitMap / chip_FreeBitMap / chip_AllocCardMem /
  chip_FreeCardMem, and chip_GetBitMapAttr.  Vtable wired via
  `chip_alloc_fill_vtable()`.
- `chip_blit.c` (~480 lines) -- chip_FillRect / chip_InvertRect /
  chip_BlitRect / chip_BlitRectNoMaskComplete (with full Minterm
  decode), chip_BlitTemplate / chip_BlitPattern (with DrawMode +
  FgPen/BgPen), chip_DrawLine (Bresenham), and chip_BlitPlanar2Chunky
  / chip_BlitPlanar2Direct.  Vtable wired via `chip_blit_fill_vtable()`.
- `chip_p96.c` (~840 lines) -- display config (SetSwitch, SetGC,
  SetPanning, SetDAC, SetColorArray, SetDisplay, SetDPMSLevel),
  pixel clock, vsync (WaitVerticalSync, GetVSyncState, GetCurrentY,
  SetInterrupt), sprite callbacks, and the now-much-shorter top-level
  `chip_fill_boardinfo_vtable` that calls the two helpers.

Pure refactor.  No behaviour change; clean build.

### Mode-resize back-buffer fix

`chip_SetGC` dims-changed path was zeroing the strip outside the new
active area in `gs->fb_mem` (front buffer) but not in `gs->fb_mem2`
(back buffer in double-buffer mode).  On the next refresh tick the
flush task swaps front<->back, leaving stale content from the old
mode visible for one frame on every resize.  Fixed: clear both
buffers when dims_changed && double_buffer.

### virtiogpu_info Shell utility

New diagnostic CLI in `src/tools/virtiogpu_info.c` (compiles to
`build/virtiogpu_info`).  Uses `IGraphics->GetBoardDataTagList` (AOS4
V54 graphics.library) to print all RTG boards' standard GBD_* fields:
BoardName, BoardDriver, ChipDriver, PCIVendorID/ProductID, total/free/
largest memory, internal-memory split.  `-v` adds memory usage % and
fragmentation calc.  Replaces -DDEBUG heartbeat for release-build users.

### Doc cleanup

Status headers added to historical RE notes pointing to the live
sources of truth:

- `chipdriver_research.md` -> `aos4_boardinfo_offsets.md` for offsets;
  the rest is foundational FindCard/InitCard reference material.
- `pcigraphics_card_analysis.md` -> historical (DSI crash trail); use
  `aos4_boardinfo_offsets.md` for current BoardInfo layout.
- `siliconmotion502_chip_analysis.md` -> reference document for chip-
  driver protocol patterns; RadeonRX.chip + graphics.library.kmod
  are better sources for live BoardInfo behaviour.
- `PHASE5B_COMPOSITING_PLAN.md` -> historical; Phase 5d compositing
  shipped in v53.73, see `chip_composite.c` for live code.

### WritePixelArray/ReadPixelArray fast path -- deferred with reasoning

Considered hooking `IGraphics->WritePixelArray` (similar to the
existing CompositeTagList hook) and detecting when the destination
is one of our RTG bitmaps for accelerated paths.  Deferred because:

1. No profile data shows P96's existing CPU loop is a bottleneck.
2. The realistic win is on format-conversion cases (RGB888->A8R8G8B8,
   CLUT->TrueColor) which require non-trivial conversion code; same-
   format writes are already fundamentally memcpy-bound.
3. Hooking adds complexity (signature, fallback, format detection)
   that's hard to size correctly without measurements.

Worth revisiting if image-viewer or compositor workloads surface
WritePixelArray as a measured hotspot.

### Build housekeeping

Note for future maintainers: this session uncovered that earlier
sessions used a WSL detour (`wsl sh -c "docker run ... -v
/mnt/w/Code/amiga/antigravity:/src ..."`) that mounted a *different*
source tree at `W:\Code\amiga\antigravity\projects\VirtIOGPU\` than
the canonical `C:\msys64\home\rich_\Projects\VirtIOGPU\`.  The two
trees drifted out of sync silently; v53.107..v53.111 edits went only
to C: and were never compiled into deployed binaries (the constant
160540-byte chip binary tipped this off).

**Resolution:** Subsequent builds invoke Docker Desktop directly
against the C: tree -- no WSL hop, no second source tree:

```
MSYS_NO_PATHCONV=1 docker run --rm \
  -v "C:/msys64/home/rich_/Projects/VirtIOGPU:/src" \
  -w "/src" walkero/amigagccondocker:os4-gcc11 \
  sh -c 'make -j$(nproc) all'
```

`MSYS_NO_PATHCONV=1` is required because Git Bash on Windows
otherwise mangles `/src/...` into `C:\Program Files\Git\src\...`.
Memory entry `build_workflow.md` updated with the canonical
invocation.

### Build

- Clean build, no new warnings.  `S:\temp\virtiogpu.chip` (v53.112,
  ~177 KB debug) and `S:\temp\virtiogpu_info` (~70 KB).

---

## v53.113 -- 26.04.2026

**Critical fix: slot table size.**

When the v53.111/.112 changes started actually being compiled (after
the C:/W: tree-divergence fix), the boot log showed Workbench failing
to come up with hundreds of repeated:

```
[virtiogpu.chip] board_alloc: alloc table full (64 slots)
[virtiogpu.chip] AllocCardMem: OUT OF BOARD MEMORY
```

Cause: `MAX_BOARD_ALLOCS = 64` in chip_alloc.c.  AOS4 graphics.library
+ rtg.library together hold ~120 concurrent AllocCardMem-style
allocations during Workbench startup (BitMapExtras, sprite save
buffers, mode info caches, etc.).  64 slots fill before Workbench
can finish coming up; subsequent AllocCardMem calls fail even though
board memory has plenty of free RAM.

The 64-slot limit had been there since v53.x but pre-v53.111 builds
were running with the **old** top-compaction allocator from W:\ that
never got my v53.107+ first-fit changes (per the C:/W: tree gotcha
caught in v53.112).  With first-fit + coalesce now actually live, the
slot count grows non-monotonically as middle-of-heap holes accumulate
before they collapse, exposing the 64-slot ceiling.

Fix: bumped `MAX_BOARD_ALLOCS` from 64 to **512**.  Per-slot cost is
~12 bytes; 512 slots = 6 KB BSS, negligible against 128 MB board
memory.  Also added a one-shot diagnostic that logs when the table
crosses 75 % full ("consider raising MAX_BOARD_ALLOCS"), so future
maintainers see the warning before the failure.

### Build

- Clean build, no new warnings.  `S:\temp\virtiogpu.chip` (v53.113,
  ~177 KB debug).

---

## v53.114 -- 26.04.2026

**Critical fix: black screen after Workbench start.**

After v53.113 fixed the slot-table ceiling, the screen still came up
black.  Boot log showed:

```
[virtiogpu.chip] SetSwitch(0) MemBase=0x5FEFF000          <-- early at boot
...
[virtiogpu.chip] SetDisplay(1) ModeInfo=0x6FF09580        <-- display ON
[virtiogpu.chip] flush_task: started ...
                                                          <-- nothing else
```

flush task alive, no host transfers being issued.

### Root cause

v53.108 added `gs->rtg_output_enabled` (set by `chip_SetSwitch`) and
gated the flush task on it.  My assumption: `SetSwitch(TRUE)` would
eventually be called to "wake up" RTG output.  Wrong for AOS4 +
QEMU Pegasos2.

Canonical Picasso96 `SetSwitch` is for physical VGA pass-through
cards toggling the display chain between native Amiga chipset video
and the RTG board.  Pegasos2 in QEMU has no native chipset to switch
*from*, so graphics.library calls `SetSwitch(FALSE)` once at boot to
indicate "RTG output not currently routed" and **never calls
`SetSwitch(TRUE)` again**.  RTG content is driven via `SetDisplay`
+ the normal flush path; SetSwitch is just a reflection of the
display-chain state.

The flush task condition

```c
if (gs->dpms_level != 0 || !gs->rtg_output_enabled) {
    continue;   // skip flush
}
```

therefore caused every wake-up after boot to skip the host transfer.
Result: black screen.

### Why this didn't bite earlier

v53.108 introduced the gate, but v53.108..v53.111 were never compiled
into deployed binaries because of the C:/W: tree drift caught in
v53.112.  v53.112 was the first build that actually ran the
`rtg_output_enabled` gate -- and that's when the black screen
started.

### Fix

Removed the `rtg_output_enabled` clause from the flush task gate.
DPMS still suppresses transfers when the display is genuinely off
(handled by `chip_SetDPMSLevel`).  `gs->rtg_output_enabled` field
kept in ChipGPUState as a stat counter; future code that genuinely
needs the SetSwitch state has access, but the flush path no longer
honours it.

### Build

- Clean build, no new warnings.  `S:\temp\virtiogpu.chip` (v53.114,
  ~177 KB debug).

---

## v53.160 -- 10.06.2026

**P96 code review vs ATIRadeon decompile: screen-drag black fix +
cursor coordinate contract**

Review driven by two user-reported bugs, validated against the
known-working ATIRadeon.chip clean-room decompile
(`Projects/Decompile/ATIRadeon`) and live tracing on QEMU amigaone.

### Screen dragging turned the whole display black (FIXED)

Root cause found by tracing the drag with per-op render logging:
when a screen is dragged, AOS4 graphics.library allocates a
full-screen scratch bitmap, fills the revealed strip black through
`FillRect`, copies the dragged screen's content into the scratch
through `BlitRectNoMaskComplete` **with opcode 0x0C**, and pans the
display to the scratch.  Our NMC decoded the opcode as a BltBitMap
minterm byte (`rop = opcode >> 4`), which turned 0x0C (= the 4-bit
logic-op VALUE for SrcCopy, the form ATIRadeon programs directly into
its rop register as `opcode & 0xf`) into 0x0 = Clear -- the content
copy painted black, so the whole dragged display was black.  The
decode now accepts both conventions: values > 0x0F are minterm bytes
(op = high nibble), values <= 0x0F are the op itself.  Verified
live: drag down shows black strip above + intact Workbench below;
drag back restores cleanly.

### Cursor / panned-origin contract (ATIRadeon-faithful)

- `chip_SetPanning` now records `bi->XOffset/YOffset` (canonical
  contract; ATIRadeon `cb_SetPanning` does exactly this).
- `chip_SetSpritePosition` ported to ATIRadeon semantics: subtracts
  the panned origin, clamps to the current `ModeInfo` dimensions (a
  stale position from a larger previous mode can no longer park the
  VirtIO cursor outside the visible mode -- the "cursor thinks the
  screen is a different resolution" symptom), and pushes
  `MOVE_CURSOR` on every position change (the historical flicker came
  from per-move image re-definition, not from MOVE_CURSOR; the
  one-shot post-SetGC refresh hack is gone).
- `chip_SetSpriteImage` applies the same transform so the two paths
  agree.
- Doublescan y-doubling from ATIRadeon deliberately NOT replicated
  (no VirtIOGPU mode sets GMF_DOUBLESCAN).

### Diagnostics

- New host-side tools (not in `make all`): `src/tools/mempeek.c`
  (summarise/dump a guest memory range from a shell) and
  `src/tools/draginput.c` (inject a scripted title-bar drag through
  input.device -- works when host-side QMP input is unavailable).
- Cursor logs (`MOVE_CURSOR`, `UPDATE_CURSOR`, `SpriteImage`) demoted
  to `DCHIP_V`: they now fire per pointer move / busy-pointer frame.

### Investigation notes

- AOS4 does NOT use SetSplitPosition, a YSplit BoardInfo field, or
  any split mechanism for screen dragging -- full composition into a
  scratch bitmap + SetPanning.  BoardInfo diff during a live drag
  shows only the Mouse fields changing.
- graphics.library brackets the pan with SetInterrupt(FALSE/TRUE)
  when `BIF_VBLANKINTERRUPT` is set, and runs a sprite-save-buffer
  blit path (CalculateMemory + buffer blit + WaitBlitter) when
  `BIF_HASSPRITEBUFFER` is set.
- QEMU `-device loader,file=` reads the kickstart image ONCE at
  process start: `system_reset` re-deploys the stale bytes.  Inject a
  new chip => full QEMU restart, not reset.
- The Makefile has no header dependencies: after touching
  `chip_state.h`, `make all` without `make clean` links objects with
  mismatched struct layouts (boots stall in graphics init).  Always
  clean-build after header changes.

---

## v53.161 -- 11.06.2026

**Soft-sprite cursor + asynchronous chip_flush (FillRect 4-178x)**

### Cursor: OS-drawn soft sprite (alignment fix)

`bi->SoftSpriteFlags` now covers every supported format, so
graphics.library draws the pointer into the framebuffer at its own
logical position.  The VirtIO hardware cursor plane is rendered by the
HOST at the HOST pointer position, while clicks land at the position
AmigaOS integrates from raw PS/2 deltas -- mouse acceleration makes the
two drift apart (user-reported: selection boxes offset from the
pointer, close gadget unhittable).  A soft sprite cannot disagree with
the click position.  BIF_HASSPRITEBUFFER dropped (rtg.library needs
its MouseSaveBuffer for the under-cursor save/restore).  The cursor
plane code stays wired; zero SoftSpriteFlags to revert.

### Performance: chip_flush is now signal-only

gfxbench2d baseline: FillRect pinned at ~400 ops/s regardless of size
(2.5 ms fixed per op) while BltBitMap ran 254k ops/s.  chip_flush()
was doing a synchronous convert + TRANSFER + RESOURCE_FLUSH round-trip
per drawing op, redundantly -- the flush task full-frames every wake
anyway (GRANTDIRECTACCESS).  Now it clips + signals only.

Measured (QEMU amigaone, 1280x800x32): FillRect 16x16 357->63,469
ops/s (178x); 512x512 299->1,232 ops/s (4.1x, memory-bound at
308 MPixel/s); Random mixed 2,407->6,028 ops/s (2.5x); blits
unchanged.

### Lab note

A boot stall reproduced after this change turned out to be hd0.qcow2
filesystem corruption from repeated hard QEMU kills during the
session, not the change -- a fresh temp image from masters boots and
benches cleanly.  Prefer fresh temp dirs after hard kills.

---

## Planned releases

| Version | Phase | Objective |
|---------|-------|-----------|
| v54.x | Phase 5b | Hardware compositing (CompositeTags) |
| v55.x | Phase 6 | MiniGL / Warp3D via Virgl 3D |
