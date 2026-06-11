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
#   make all       — chip driver, minigl stub, helper tools (default)
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
CHIP_CFLAGS  = -O2 -Wall -I./include -fno-tree-loop-distribute-patterns -DDEBUG \
               -mcrt=newlib -D__NOLIBBASE__ -D__NOGLOBALIFACE__ $(DEPFLAGS)
CHIP_LDFLAGS = -mcrt=newlib -nostartfiles

# MiniGL stub library uses the same flags as the chip
MGL_CFLAGS   = -O2 -Wall -I./include -fno-tree-loop-distribute-patterns \
               -mcrt=newlib -D__NOLIBBASE__ -D__NOGLOBALIFACE__ $(DEPFLAGS)
MGL_LDFLAGS  = -mcrt=newlib -nostartfiles

# Docker image used to run lha when it is not on the host PATH
DOCKER_IMAGE ?= walkero/amigagccondocker:os4-gcc11
DOCKER_RUN    = docker run --rm -v "$(CURDIR):/work" -w /work $(DOCKER_IMAGE)

BUILD_DIR   = build
CHIP_TARGET = $(BUILD_DIR)/virtiogpu.chip
MGL_TARGET  = $(BUILD_DIR)/minigl.library
TOOL_TARGET = $(BUILD_DIR)/setup_monitor
STUB_TARGET = $(BUILD_DIR)/monitor_stub
COMP_TARGET = $(BUILD_DIR)/test_composite
INFO_TARGET = $(BUILD_DIR)/virtiogpu_info

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
               src/chip/chip_flush.c \
               src/chip/chip_p96.c \
               src/chip/chip_alloc.c \
               src/chip/chip_blit.c \
               src/chip/chip_modes.c \
               src/chip/chip_board.c \
               src/chip/chip_irq.c \
               src/chip/chip_init.c
CHIP_OBJ     = $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(CHIP_SRC))
CHIP_VQ_OBJ  = $(BUILD_DIR)/chip/virtqueue_chip.o

MGL_SRC      = src/minigl/minigl_lib.c
MGL_OBJ      = $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(MGL_SRC))

DEP = $(CHIP_OBJ:.o=.d) $(CHIP_VQ_OBJ:.o=.d) $(MGL_OBJ:.o=.d)

.PHONY: all clean dist dist-lha help

all: $(CHIP_TARGET) $(MGL_TARGET) $(TOOL_TARGET) $(STUB_TARGET) \
     $(COMP_TARGET) $(INFO_TARGET)

$(CHIP_TARGET): $(CHIP_OBJ) $(CHIP_VQ_OBJ)
	$(CC) $(CHIP_OBJ) $(CHIP_VQ_OBJ) -o $(CHIP_TARGET) $(CHIP_LDFLAGS)

$(BUILD_DIR)/chip/%.o: src/chip/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CHIP_CFLAGS) -c $< -o $@

$(BUILD_DIR)/chip/virtqueue_chip.o: src/virtio/virtqueue.c
	@mkdir -p $(dir $@)
	$(CC) $(CHIP_CFLAGS) -c $< -o $@

$(MGL_TARGET): $(MGL_OBJ)
	$(CC) $(MGL_OBJ) -o $(MGL_TARGET) $(MGL_LDFLAGS)

$(BUILD_DIR)/minigl/%.o: src/minigl/%.c
	@mkdir -p $(dir $@)
	$(CC) $(MGL_CFLAGS) -c $< -o $@

$(TOOL_TARGET): src/tools/setup_monitor.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall $< -o $@ -lauto

$(STUB_TARGET): src/tools/monitor_stub.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall $< -o $@ -lauto

$(COMP_TARGET): src/tools/test_composite.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall $< -o $@ -lauto

$(INFO_TARGET): src/tools/virtiogpu_info.c
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
	cp -f README.md                               $(DIST_STAGE)/README.md
	@echo "=== Staged distribution drawer ==="
	@find $(DIST_STAGE) -type f | sort

dist-lha: dist
	rm -f $(DIST_LHA)
	@if command -v lha >/dev/null 2>&1; then \
	    (cd $(DIST_DIR) && lha ao5q ../VirtIOGPU.lha VirtIOGPU); \
	else \
	    echo "lha not on PATH — packing inside Docker"; \
	    $(DOCKER_RUN) sh -c 'cd $(DIST_DIR) && lha ao5q /work/$(DIST_LHA) VirtIOGPU'; \
	fi
	@ls -la $(DIST_LHA)

help:
	@echo "virtiogpu.chip build system"
	@echo ""
	@echo "  make all       - chip driver, minigl stub, helper tools (default)"
	@echo "  make dist      - stage Installation Utility drawer in $(DIST_DIR)/"
	@echo "  make dist-lha  - pack the drawer into $(DIST_LHA)"
	@echo "  make clean     - remove $(BUILD_DIR)/"
	@echo ""
	@echo "Build via Docker (run clean and all separately):"
	@echo "  docker run --rm -v \"\$$(pwd):/src\" -w /src $(DOCKER_IMAGE) make clean"
	@echo "  docker run --rm -v \"\$$(pwd):/src\" -w /src $(DOCKER_IMAGE) sh -c 'make -j\$$(nproc) all'"
