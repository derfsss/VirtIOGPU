# ---------------------------------------------------------------------------
# Makefile — virtiogpu.chip (Picasso96 RTG driver) for AmigaOS 4.1 FE
#
# Requires the AmigaOS 4 cross-toolchain (ppc-amigaos-gcc), easiest via
# the public Docker image:
#
#   docker run --rm -v "$(pwd):/src" -w /src \
#       walkero/amigagccondocker:os4-gcc11 make clean
#   docker run --rm -v "$(pwd):/src" -w /src \
#       walkero/amigagccondocker:os4-gcc11 sh -c 'make -j$(nproc) all'
#
# (Run clean and all as SEPARATE invocations — a combined parallel build
# races: clean deletes the build dir while compilation is running.)
#
# Targets:
#   make all       — chip driver + helper tools (default)
#   make dist      — stage the Installation Utility drawer in build/dist/
#   make dist-lha  — pack the staged drawer into build/VirtIOGPU.lha
#   make clean     — remove build/
#   make help      — this summary
# ---------------------------------------------------------------------------

CC = ppc-amigaos-gcc

# Chip driver needs -mcrt=newlib so AmigaOS4 library/interface
# infrastructure (CLT_*, GetInterface, TOC pointer) works correctly.
# -MMD -MP emits .d files so header edits retrigger the right TUs.
DEPFLAGS     = -MMD -MP
CHIP_CFLAGS  = -O2 -Wall -I./include -I./include/gpulib \
               -fno-tree-loop-distribute-patterns -DDEBUG \
               -mcrt=newlib -D__NOLIBBASE__ -D__NOGLOBALIFACE__ $(DEPFLAGS)
# `make SHIM_CONTRACT=1 ...` builds the P96_Replacement Phase 4
# contract-only shim variant (frozen SHIM_CONTRACT.md blit subset;
# everything else keeps the PCIGraphics.card soft defaults).
ifdef SHIM_CONTRACT
CHIP_CFLAGS += -DGPU_SHIM_CONTRACT
endif
# `make VTABLE_PROBE=1 ...` dumps the 68 BoardInfo vtable slots as
# PCIGraphics.card hands them over, before the chip assigns any
# (P96_Replacement Phase 9 Step 0).  Diagnostic only -- do not ship.
ifdef VTABLE_PROBE
CHIP_CFLAGS += -DGPU_VTABLE_PROBE
endif
CHIP_LDFLAGS = -mcrt=newlib -nostartfiles

# Docker image used to run lha when it is not on the host PATH
DOCKER_IMAGE ?= walkero/amigagccondocker:os4-gcc11
DOCKER_RUN    = docker run --rm -v "$(CURDIR):/work" -w /work $(DOCKER_IMAGE)

BUILD_DIR   = build
CHIP_TARGET = $(BUILD_DIR)/virtiogpu.chip
COMP_TARGET = $(BUILD_DIR)/test_composite
INFO_TARGET = $(BUILD_DIR)/virtiogpu_info
GPUVTEST_TARGET = $(BUILD_DIR)/gpu_vtest
W3DTRI_TARGET = $(BUILD_DIR)/w3dtri
W3DSUITE_TARGET = $(BUILD_DIR)/w3d_suite
W3DPRESENT_TARGET = $(BUILD_DIR)/w3d_present

# -----------------------------------------------------------------------
# Chip driver sources (Picasso96 .chip plugin)
# virtqueue.c is compiled again with CHIP_CFLAGS (-mcrt=newlib) into
# a chip-specific object so the chip is fully self-contained.
# -----------------------------------------------------------------------
CHIP_SRC     = src/chip/chip_lib.c \
               src/chip/chip_gpu_cmds.c \
               src/chip/chip_gpu_3d.c \
               src/chip/chip_virgl.c \
               src/chip/chip_virgl_2d.c \
               src/chip/chip_composite.c \
               src/chip/chip_vram.c \
               src/chip/chip_flush.c \
               src/chip/chip_p96.c \
               src/chip/chip_alloc.c \
               src/chip/chip_blit.c \
               src/chip/chip_modes.c \
               src/chip/chip_board.c \
               src/chip/chip_irq.c \
               src/chip/chip_perf.c \
               src/chip/chip_v3d.c \
               src/chip/chip_gpu_srv.c \
               src/chip/chip_gpu_backend.c \
               src/chip/chip_init.c
CHIP_OBJ     = $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(CHIP_SRC))
CHIP_VQ_OBJ  = $(BUILD_DIR)/chip/virtqueue_chip.o

# -----------------------------------------------------------------------
# warp3d.library (GPL) -- Warp3D V5 API over the chip's "v3d" transport.
# Reuses src/chip/chip_virgl.c compiled with -DVIRGL_ENCODE_ONLY (chip-free
# encoder; no ChipGPUState / virgl_submit).  Needs the SDK warp3d headers.
# -----------------------------------------------------------------------
W3D_TARGET   = $(BUILD_DIR)/warp3d.library
W3D_CFLAGS   = -O2 -Wall -I./include -I./include/gpulib -fno-tree-loop-distribute-patterns \
               -mcrt=newlib -D__NOLIBBASE__ -D__NOGLOBALIFACE__ $(DEPFLAGS)
W3D_LDFLAGS  = -mcrt=newlib -nostartfiles
W3D_OBJ      = $(BUILD_DIR)/warp3d/warp3d_lib.o \
               $(BUILD_DIR)/warp3d/warp3d_main.o \
               $(BUILD_DIR)/warp3d/virgl_encode.o

# -----------------------------------------------------------------------
# W3D_VirtIOGPU.library (Phase 8 Part B) -- Warp3D V5 HW BACKEND loaded by the
# stock Warp3D.library FE 53.27 from LIBS:Warp3D/HWdrivers/.  B1 = probe skeleton.
# -----------------------------------------------------------------------
W3DVIO_TARGET  = $(BUILD_DIR)/W3D_VirtIOGPU.library
W3DVIO_CFLAGS  = -O2 -Wall -I./include -I./include/gpulib -fno-tree-loop-distribute-patterns \
                 -mcrt=newlib -D__NOLIBBASE__ -D__NOGLOBALIFACE__ $(DEPFLAGS)
W3DVIO_LDFLAGS = -mcrt=newlib -nostartfiles
# B-hw2: the HW backend now compiles in the proven virgl render core
# (warp3d_main.c) + the chip-free virgl encoder (chip_virgl.c -DVIRGL_ENCODE_ONLY).
W3DVIO_OBJ     = $(BUILD_DIR)/w3d_virtio/w3d_virtio_lib.o \
                 $(BUILD_DIR)/w3d_virtio/warp3d_main.o \
                 $(BUILD_DIR)/w3d_virtio/virgl_encode.o

DEP = $(CHIP_OBJ:.o=.d) $(CHIP_VQ_OBJ:.o=.d) $(W3D_OBJ:.o=.d) $(W3DVIO_OBJ:.o=.d)

.PHONY: all clean dist dist-lha help

all: $(CHIP_TARGET) $(W3D_TARGET) $(W3DVIO_TARGET) $(COMP_TARGET) $(INFO_TARGET) $(W3DTRI_TARGET) $(W3DSUITE_TARGET) $(GPUVTEST_TARGET)

$(W3DVIO_TARGET): $(W3DVIO_OBJ)
	$(CC) $(W3DVIO_OBJ) -o $(W3DVIO_TARGET) $(W3DVIO_LDFLAGS)

$(BUILD_DIR)/w3d_virtio/%.o: src/w3d_virtio/%.c
	@mkdir -p $(dir $@)
	$(CC) $(W3DVIO_CFLAGS) -c $< -o $@

# render core + chip-free virgl encoder, compiled into the HW backend
$(BUILD_DIR)/w3d_virtio/warp3d_main.o: src/warp3d/warp3d_main.c
	@mkdir -p $(dir $@)
	$(CC) $(W3DVIO_CFLAGS) -c $< -o $@

$(BUILD_DIR)/w3d_virtio/virgl_encode.o: src/chip/chip_virgl.c
	@mkdir -p $(dir $@)
	$(CC) $(W3DVIO_CFLAGS) -DVIRGL_ENCODE_ONLY -c $< -o $@

# W3D_VirtIO.library -- Warp3D GFX DRIVER (Phase 8 B-gfx1), loaded from
# LIBS:Warp3D/GFXdrivers/.  Same flags as the HW backend.  B-gfx1 = probe.
$(BUILD_DIR)/W3D_VirtIO.library: $(BUILD_DIR)/w3d_virtio_gfx/w3d_virtio_gfx_lib.o
	$(CC) $< -o $@ $(W3DVIO_LDFLAGS)

$(BUILD_DIR)/w3d_virtio_gfx/%.o: src/w3d_virtio_gfx/%.c
	@mkdir -p $(dir $@)
	$(CC) $(W3DVIO_CFLAGS) -c $< -o $@

$(CHIP_TARGET): $(CHIP_OBJ) $(CHIP_VQ_OBJ)
	$(CC) $(CHIP_OBJ) $(CHIP_VQ_OBJ) -o $(CHIP_TARGET) $(CHIP_LDFLAGS)

$(BUILD_DIR)/chip/%.o: src/chip/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CHIP_CFLAGS) -c $< -o $@

$(BUILD_DIR)/chip/virtqueue_chip.o: src/virtio/virtqueue.c
	@mkdir -p $(dir $@)
	$(CC) $(CHIP_CFLAGS) -c $< -o $@

$(W3D_TARGET): $(W3D_OBJ)
	$(CC) $(W3D_OBJ) -o $(W3D_TARGET) $(W3D_LDFLAGS)

$(BUILD_DIR)/warp3d/%.o: src/warp3d/%.c
	@mkdir -p $(dir $@)
	$(CC) $(W3D_CFLAGS) -c $< -o $@

$(BUILD_DIR)/warp3d/virgl_encode.o: src/chip/chip_virgl.c
	@mkdir -p $(dir $@)
	$(CC) $(W3D_CFLAGS) -DVIRGL_ENCODE_ONLY -c $< -o $@

$(COMP_TARGET): src/tools/test_composite.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall $< -o $@ -lauto

$(INFO_TARGET): src/tools/virtiogpu_info.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall $< -o $@ -lauto

$(GPUVTEST_TARGET): src/tools/gpu_vtest.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -I./include -I./include/gpulib $< -o $@ -lauto

$(W3DTRI_TARGET): src/tools/w3dtri.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall $< -o $@ -lauto

$(W3DSUITE_TARGET): src/tools/w3d_suite.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall $< -o $@ -lauto

$(W3DPRESENT_TARGET): src/tools/w3d_present.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall $< -o $@ -lauto

-include $(DEP)

clean:
	rm -rf $(BUILD_DIR)

# -----------------------------------------------------------------------
# Distribution
#
# `make dist`     — stage an AmigaOS Installation Utility drawer under
#                   build/dist/VirtIOGPU/ (chip + installer + icon +
#                   readme).
# `make dist-lha` — pack the staged drawer into build/VirtIOGPU.lha.
#                   Uses host lha when available, otherwise runs lha
#                   inside the toolchain Docker image.
#
# installer/install.py + installer/VirtIOGPUInstallerLocale.py are
# pre-generated and committed, so dist needs no extra tooling.  On the
# Amiga the user double-clicks the install.py icon (its default tool is
# the OS Installation Utility) or runs:
#     "SYS:Utilities/Installation Utility" PACKAGE=install.py
# from a shell with the drawer as the current directory.
# -----------------------------------------------------------------------
DIST_DIR   = $(BUILD_DIR)/dist
DIST_STAGE = $(DIST_DIR)/VirtIOGPU
DIST_LHA   = $(BUILD_DIR)/VirtIOGPU.lha

dist: all
	rm -rf $(DIST_STAGE)
	mkdir -p $(DIST_STAGE)/content
	cp -f $(CHIP_TARGET)                          $(DIST_STAGE)/content/virtiogpu.chip
	cp -f installer/install.py                    $(DIST_STAGE)/install.py
	cp -f installer/install.py.info               $(DIST_STAGE)/install.py.info
	cp -f installer/VirtIOGPUInstallerLocale.py   $(DIST_STAGE)/VirtIOGPUInstallerLocale.py
	cp -f installer/drawer.info                   $(DIST_DIR)/VirtIOGPU.info
	cp -f README.md                               $(DIST_STAGE)/README.md
	@echo "=== Staged distribution drawer ==="
	@find $(DIST_STAGE) $(DIST_DIR)/VirtIOGPU.info -type f | sort

dist-lha: dist
	rm -f $(DIST_LHA)
	@if command -v lha >/dev/null 2>&1; then \
	    (cd $(DIST_DIR) && lha ao5q ../VirtIOGPU.lha VirtIOGPU VirtIOGPU.info); \
	else \
	    echo "lha not on PATH — packing inside Docker"; \
	    $(DOCKER_RUN) sh -c 'cd $(DIST_DIR) && lha ao5q /work/$(DIST_LHA) VirtIOGPU VirtIOGPU.info'; \
	fi
	@ls -la $(DIST_LHA)

help:
	@echo "virtiogpu.chip build system"
	@echo ""
	@echo "  make all       - chip driver + helper tools (default)"
	@echo "  make dist      - stage Installation Utility drawer in $(DIST_DIR)/"
	@echo "  make dist-lha  - pack the drawer into $(DIST_LHA)"
	@echo "  make clean     - remove $(BUILD_DIR)/"
	@echo ""
	@echo "Build via Docker (run clean and all separately):"
	@echo "  docker run --rm -v \"\$$(pwd):/src\" -w /src $(DOCKER_IMAGE) make clean"
	@echo "  docker run --rm -v \"\$$(pwd):/src\" -w /src $(DOCKER_IMAGE) sh -c 'make -j\$$(nproc) all'"
