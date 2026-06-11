# virtiogpu.chip

A Picasso96 RTG graphics driver for AmigaOS 4.1 Final Edition on the QEMU VirtIO GPU.

**Status:** Beta (v53.161) — Workbench-ready on QEMU Pegasos2 and AmigaOne; Sam460 planned.

> ⚠️ **Beta — actively under development.** Expect bugs and rough
> edges; do not rely on it for anything important. Use at your own
> risk.

---

## Overview

`virtiogpu.chip` drives the VirtIO GPU (PCI `1AF4:1050`) provided by
QEMU, giving AmigaOS 4.1 FE full RTG support: 8/16/32-bit screen
modes, EDID-based native resolution detection, dynamic mode switching,
screen dragging, and Virgl 2D/3D GPU acceleration.

Recent releases fixed screen dragging, aligned the pointer with the
click position (OS-drawn soft sprite), and made small fills up to two
orders of magnitude faster (asynchronous flush) — see
[docs/VERSIONS.md](docs/VERSIONS.md) for the full history.

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

---

## Features

- **8/16/32-bit colour** — CLUT8, R5G6B5, and B8G8R8X8 pixel formats.
- **Screen modes** — standard SVGA/HD modes plus the EDID native
  resolution; the framebuffer resource is recreated to match the
  active mode on each screen-mode change.
- **Screen dragging** — full support for AmigaOS dragged-screen
  composition.
- **Pointer** — OS-drawn soft sprite, so the rendered cursor always
  matches the click position under QEMU's relative mouse input.  The
  VirtIO hardware-cursor plane is implemented but disabled by default
  (host-rendered cursors drift from the guest pointer position).
- **EDID support** — `GET_EDID` for native resolution detection.
- **DPMS** — display power management.
- **Virgl 2D acceleration** — FillRect via `CLEAR`, BlitRect via
  `BLIT` (GPU-side), when QEMU exposes Virgl (`virtio-gpu-gl-pci`).
- **Hardware compositing** — `CompositeTags` hook with Porter-Duff
  blend states.

---

## Requirements

- AmigaOS 4.1 Final Edition.
- QEMU with `-M pegasos2` or `-M amigaone` and a VirtIO GPU display
  device (see below).

---

## QEMU Setup

```
-M pegasos2 -device virtio-gpu-pci
```

or, for Virgl GPU acceleration:

```
-M pegasos2 -device virtio-gpu-gl-pci -display sdl,gl=on
```

The Pegasos2 MV64361 bridge transparently maps PCI BAR addresses into
the CPU address space, allowing direct MMIO access via
`lwbrx`/`stwbrx`.

**QEMU AmigaOne** works the same way:

```
-M amigaone -device virtio-gpu-pci
```

AmigaOS's AmigaOne PCI enumeration leaves the high dword of 64-bit
BARs unprogrammed (the "AmigaOne PCI probe bug"); the driver repairs
the BAR before its scan, after which direct MMIO works.  This applies
to the QEMU machine only — real Articia S silicon does not forward
PCI MMIO, so real-hardware support would need the scaffolded PCI_CFG
fallback (untested).

**Sam460** is a planned future target.  On an unsupported machine the
driver fails cleanly at init with a diagnostic message rather than
crashing.

---

## Installation

Copy `build/virtiogpu.chip` into `SYS:Kickstart/` on your AmigaOS
volume, then add it to `SYS:Kickstart/Kicklayout` **before**
`PCIGraphics.card`:

```
MODULE Kickstart/virtiogpu.chip
MODULE Kickstart/PCIGraphics.card
```

No configuration is required.  The driver queries QEMU for the active
display via `GET_DISPLAY_INFO` and EDID, and registers screen modes
automatically.

Note that bboot-based setups load Kickstart from `kickstart.zip`, not
from `SYS:Kickstart/Kicklayout` — on those, add the chip and the
Kicklayout line to the zip instead.

### Distribution archive

`make dist` stages an AmigaOS **Installation Utility** drawer under
`build/dist/VirtIOGPU/` (chip + installer script + readme);
`make dist-lha` packs it into `build/VirtIOGPU.lha`.

The bundled `install.py` targets the Python-based Installation Utility
shipped with every AmigaOS 4.1 Final Edition: it copies the chip to
`SYS:Kickstart/`, inserts the `MODULE` line into Kicklayout (backing
up `Kicklayout.bak` first, idempotently), and offers a reboot.
**Launch it from inside the extracted drawer** (double-click, or `CD`
into the drawer first) — the script uses drawer-relative `content/`
paths, like the OS's own update installers.

One QEMU quirk: the "Reboot now" option on the finish page may power
the virtual machine off instead of restarting it — simply start QEMU
again.

`installer/install.py` is pre-generated from
`installer/virtiogpu_installer_fixture.py` and committed, so building
the archive needs no extra tooling.

---

## Building from Source

Requires the `walkero/amigagccondocker:os4-gcc11` Docker image
(provides `ppc-amigaos-gcc`, GCC 11).

Run `clean` and `all` as **separate** Docker invocations — a combined
parallel build races (clean deletes the build dir while compilation
runs):

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

## Project Structure

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

### Architecture notes

The chip binary contains two AmigaOS residents:

- **VirtIOGPUBoard** (`NT_RESOURCE`, pri 65) — runs before
  graphics.library and hooks `PCIGraphics.card`'s `FindCard` /
  `InitBoard` methods via `SetMethod`.
- **virtiogpu.chip** (`NT_LIBRARY`, pri -128) — loaded on demand for
  GPU init and the Picasso96 BoardInfo vtable.

Key constraints: the chip is built with `-mcrt=newlib`; only the
modern VirtIO transport exists for device `0x1050`; MMIO uses
`stwbrx`/`lwbrx` inline assembly; two virtqueues (control + cursor).

Command flow — 2D mode:

```
RESOURCE_CREATE_2D → ATTACH_BACKING → SET_SCANOUT
TRANSFER_TO_HOST_2D + RESOURCE_FLUSH   (signal-driven flush task)
```

Virgl 2D mode (GPU-accelerated):

```
CTX_CREATE → RESOURCE_CREATE_3D → ATTACH_BACKING → SET_SCANOUT
Virgl CLEAR (FillRect) / Virgl BLIT (BlitRect) + RESOURCE_FLUSH
TRANSFER_TO_HOST_3D + RESOURCE_FLUSH   (periodic flush for GRANTDIRECTACCESS)
```

All commands are serialised by `io_lock` and use two shared 4KB DMA
buffers (`cmd_buf` / `resp_buf`).

---

## Documentation

- [docs/VERSIONS.md](docs/VERSIONS.md) — version history and release
  notes.

---

## Development

This driver was developed with [Claude](https://claude.ai) (Anthropic)
acting as the primary engineer — writing the C code, designing the
architecture, debugging hardware-level issues, and navigating the
AmigaOS 4.1 SDK — with a human developer directing, reviewing, and
testing the result.

## License

Copyright © 2026. All rights reserved.
