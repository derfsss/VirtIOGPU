# VirtIOGPU — VirtIO GPU Driver for AmigaOS 4.1FE

> ⚠️ **BETA — actively under development.** This driver is a work in
> progress. Things are incomplete, may break, or may not work at all.
> Expect bugs, crashes, and rough edges. Do not rely on it for anything
> important. Use at your own risk.

**Status: Beta.**

A native AmigaOS 4.1 Picasso96 chip driver for the VirtIO GPU
(PCI `1AF4:1050`) as provided by QEMU. Boots Workbench with full RTG
support — 8/16/32-bit screen modes, hardware cursor, EDID-based native
resolution detection, dynamic mode switching, and Virgl 2D/3D GPU
acceleration.

The current aim is to get the driver fully working on the **QEMU Pegasos2**
machine before leaving beta. The **QEMU AmigaOne** machine also works
(see [Platform](#platform) below); **Sam460** is a planned future target.

---

## Status

| Phase | Description | Status |
|-------|-------------|--------|
| Phase 1 | Basic 32bpp framebuffer at boot | Complete |
| Phase 2 | Flush command + hardware cursor | Complete |
| Phase 3 | RTG chip driver — Workbench on GPU | Complete |
| Phase 4 | EDID, screen modes, 16bpp, DPMS, mode switching | Complete |
| Phase 5 | Virgl 2D acceleration (CLEAR/BLIT) | Working |
| Phase 5b | Virgl 3D rendering (DRAW_VBO) | Working |
| Phase 5c | Double-buffering (tear-free) | Working |
| Phase 5d | Compositing (CompositeTags) | Working |
| Phase 6 | MiniGL / Warp3D via Virgl 3D | Planned |

The driver is in **beta**: it boots and runs Workbench reliably on
Pegasos2 QEMU, but is still being stabilised before a 1.0 release.

---

## Platform

The beta targets the **QEMU Pegasos2** machine:

```
-M pegasos2 -device virtio-gpu-pci
```

or, for Virgl GPU acceleration:

```
-M pegasos2 -device virtio-gpu-gl-pci -display sdl,gl=on
```

The Pegasos2 MV64361 bridge transparently maps PCI BAR addresses into the
CPU address space, allowing direct MMIO access via `lwbrx`/`stwbrx`.

**QEMU AmigaOne** also works:

```
-M amigaone -device virtio-gpu-pci
```

AmigaOS's AmigaOne PCI enumeration leaves the high dword of 64-bit BARs
unprogrammed (the "AmigaOne PCI probe bug"); the driver repairs the BAR
before its scan, after which direct MMIO works and the full init
succeeds.  Verified: 1280x800 Workbench on `-M amigaone`.  Note this
applies to the QEMU machine only — real Articia S silicon does not
forward PCI MMIO, so real-hardware support would need the scaffolded
PCI_CFG fallback (untested).

**Future QEMU targets (not yet supported):**

- **Sam460**

On an unsupported machine the driver fails cleanly at init with a
diagnostic message rather than crashing.

---

## Requirements

- **QEMU machine**: `-M pegasos2` with `-device virtio-gpu-pci` (or
  `-device virtio-gpu-gl-pci` for Virgl acceleration).
- **OS**: AmigaOS 4.1 Final Edition.

---

## Installation

Copy `build/virtiogpu.chip` into `SYS:Kickstart/` on your AmigaOS volume,
then add it to `SYS:Kickstart/Kicklayout` **before** `PCIGraphics.card`:

```
MODULE Kickstart/virtiogpu.chip
MODULE Kickstart/PCIGraphics.card
```

The chip binary contains two residents:

- **VirtIOGPUBoard** (`NT_RESOURCE`, pri 65) — runs before graphics.library,
  hooks `PCIGraphics.card`'s `FindCard` / `InitBoard` methods via `SetMethod`.
- **virtiogpu.chip** (`NT_LIBRARY`, pri -128) — loaded on demand for GPU
  init and the Picasso96 BoardInfo vtable.

No configuration is required. The driver queries QEMU for the active
display via `GET_DISPLAY_INFO` and EDID, and registers screen modes
automatically.

---

## Building

Requires the `walkero/amigagccondocker:os4-gcc11` Docker image (provides
`ppc-amigaos-gcc`, GCC 11).

Run `clean` and `all` as **separate** Docker invocations — a combined
parallel build races (clean deletes the build dir while compilation runs):

```sh
docker run --rm -v "$(pwd):/src" -w /src \
    walkero/amigagccondocker:os4-gcc11 make clean

docker run --rm -v "$(pwd):/src" -w /src \
    walkero/amigagccondocker:os4-gcc11 sh -c 'make -j$(nproc) all'
```

Outputs in `build/`:

- `virtiogpu.chip` — the Picasso96 chip driver (primary artefact)
- `minigl.library` — Phase 6 MiniGL stub (placeholder)
- `setup_monitor`, `monitor_stub`, `test_composite`, `virtiogpu_info`
  — helper / diagnostic tools

---

## Architecture

The driver is a single component, `virtiogpu.chip`, built from a
two-resident architecture that uses `SetMethod` hooks on `PCIGraphics.card`:

```
src/chip/
├── chip_lib.c          — Library boilerplate, ROM tags, _start asm
├── chip_board.c        — VirtIOGPUBoard resident (pri 65), SetMethod hooks
├── chip_init.c         — PCI/VirtIO init, chip_InitCard_C, framebuffer resize
├── chip_gpu_cmds.c     — Shared 2D GPU command wrappers + virtqueue I/O
├── chip_gpu_3d.c       — GPU 3D commands (CTX, SUBMIT_3D, RESOURCE_BLOB)
├── chip_irq.c          — IRQ install + ISR (signals waiters; polling fallback)
├── chip_flush.c        — Framebuffer convert + signal-driven flush task
├── chip_p96.c          — P96 BoardInfo vtable: display cfg, sprite, vsync
├── chip_alloc.c        — P96 bitmap alloc/free + AllocCardMem
├── chip_blit.c         — P96 FillRect / BlitRect / Template / Pattern / etc.
├── chip_modes.c        — Screen mode table + registration
├── chip_virgl.c        — Virgl capability query, feature negotiation
├── chip_virgl_2d.c     — Virgl 2D acceleration (CLEAR/BLIT, 3D scanout)
└── chip_composite.c    — CompositeTags hook + Porter-Duff blend states

include/chip/chip_state.h  — ChipGPUState struct, MMIO inlines, macros
include/virgl/virgl_cmd.h  — Virgl command buffer builder API
```

An earlier standalone `virtiogpu.device` driver (Phases 1–2) has been
removed — the chip driver does its own VirtIO init via the `SetMethod`
hooks, so no separate device is needed.

### Key constraints

- **Chip uses `-mcrt=newlib`** 
- **Modern VirtIO only** — PCI device ID `0x1050`; no legacy transport.
- **MMIO via `stwbrx` / `lwbrx`**
- **Two virtqueues** — queue 0 (control), queue 1 (cursor).

---

## Features

- **8/16/32-bit colour** — CLUT8, R5G6B5, and B8G8R8X8 pixel formats.
- **Screen modes** — standard SVGA/HD modes plus the EDID native
  resolution; the framebuffer resource is recreated to match the active
  mode on each screen-mode change.
- **Hardware cursor** — VirtIO GPU cursor plane (64×64 ARGB).
- **EDID support** — `GET_EDID` for native resolution detection.
- **DPMS** — display power management.
- **Virgl 2D acceleration** — FillRect via `CLEAR`, BlitRect via `BLIT`
  (GPU-side), when QEMU exposes Virgl (`virtio-gpu-gl-pci`).
- **Hardware compositing** — `CompositeTags` hook with Porter-Duff blend
  states.

---

## VirtIO GPU Command Flow

### 2D mode (standard)

```
RESOURCE_CREATE_2D → ATTACH_BACKING → SET_SCANOUT
TRANSFER_TO_HOST_2D + RESOURCE_FLUSH   (periodic flush task)
```

### Virgl 2D mode (GPU-accelerated)

```
CTX_CREATE → RESOURCE_CREATE_3D → ATTACH_BACKING → SET_SCANOUT
Virgl CLEAR (FillRect) / Virgl BLIT (BlitRect) + RESOURCE_FLUSH
TRANSFER_TO_HOST_3D + RESOURCE_FLUSH   (periodic flush for GRANTDIRECTACCESS)
```

All commands are serialised by `io_lock` and use two shared 4KB DMA
buffers (`cmd_buf` / `resp_buf`).

---

## Documentation

- [docs/VERSIONS.md](docs/VERSIONS.md) — version history and release notes.
