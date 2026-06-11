CC     = ppc-amigaos-gcc

# Chip driver needs mcrt=newlib so AmigaOS4 library/interface infrastructure
# (CLT_*, GetInterface, TOC pointer) works correctly
CHIP_CFLAGS  = -O2 -Wall -I./include -fno-tree-loop-distribute-patterns -DDEBUG \
               -mcrt=newlib -D__NOLIBBASE__ -D__NOGLOBALIFACE__
CHIP_LDFLAGS = -mcrt=newlib -nostartfiles

# MiniGL library uses same flags as chip (mcrt=newlib, nostartfiles)
MGL_CFLAGS   = -O2 -Wall -I./include -fno-tree-loop-distribute-patterns \
               -mcrt=newlib -D__NOLIBBASE__ -D__NOGLOBALIFACE__
MGL_LDFLAGS  = -mcrt=newlib -nostartfiles

BUILD_DIR = build
CHIP_TARGET = $(BUILD_DIR)/virtiogpu.chip
MGL_TARGET  = $(BUILD_DIR)/minigl.library

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

TOOL_TARGET = $(BUILD_DIR)/setup_monitor
STUB_TARGET = $(BUILD_DIR)/monitor_stub
COMP_TARGET = $(BUILD_DIR)/test_composite
INFO_TARGET = $(BUILD_DIR)/virtiogpu_info

DIRS = $(BUILD_DIR) $(BUILD_DIR)/chip $(BUILD_DIR)/minigl $(BUILD_DIR)/tools

all:
	mkdir -p $(DIRS)
	$(MAKE) $(CHIP_TARGET)
	$(MAKE) $(MGL_TARGET)
	$(MAKE) $(TOOL_TARGET)
	$(MAKE) $(STUB_TARGET)
	$(MAKE) $(COMP_TARGET)
	$(MAKE) $(INFO_TARGET)

$(CHIP_TARGET): $(CHIP_OBJ) $(CHIP_VQ_OBJ)
	$(CC) $(CHIP_OBJ) $(CHIP_VQ_OBJ) -o $(CHIP_TARGET) $(CHIP_LDFLAGS)

$(BUILD_DIR)/chip/%.o: src/chip/%.c
	$(CC) $(CHIP_CFLAGS) -c $< -o $@

$(BUILD_DIR)/chip/virtqueue_chip.o: src/virtio/virtqueue.c
	$(CC) $(CHIP_CFLAGS) -c $< -o $@

$(MGL_TARGET): $(MGL_OBJ)
	$(CC) $(MGL_OBJ) -o $(MGL_TARGET) $(MGL_LDFLAGS)

$(BUILD_DIR)/minigl/%.o: src/minigl/%.c
	$(CC) $(MGL_CFLAGS) -c $< -o $@

$(TOOL_TARGET): src/tools/setup_monitor.c
	$(CC) -O2 -Wall -lauto $< -o $@

$(STUB_TARGET): src/tools/monitor_stub.c
	$(CC) -O2 -Wall -lauto $< -o $@

$(COMP_TARGET): src/tools/test_composite.c
	$(CC) -O2 -Wall $< -o $@ -lauto

$(INFO_TARGET): src/tools/virtiogpu_info.c
	$(CC) -O2 -Wall $< -o $@ -lauto

clean:
	rm -rf $(BUILD_DIR)

# -----------------------------------------------------------------------
# Distribution
#
# `make dist`     -- stage an AmigaOS Installation Utility drawer under
#                    build/dist/VirtIOGPU/ (chip + installer + readme).
# `make dist-lha` -- pack the staged drawer into build/VirtIOGPU.lha.
#                    Run inside the toolchain Docker image (provides lha).
#
# installer/install.py + installer/VirtIOGPUInstallerLocale.py are
# pre-generated from installer/virtiogpu_installer_fixture.py and
# committed, so dist needs no extra tooling.  On the Amiga the archive is installed by double-clicking
# install.py or running:
#     Sys:Utilities/Installation Utility PACKAGE=install.py
# -----------------------------------------------------------------------
DIST_DIR   = $(BUILD_DIR)/dist
DIST_STAGE = $(DIST_DIR)/VirtIOGPU
DIST_LHA   = $(BUILD_DIR)/VirtIOGPU.lha

.PHONY: all clean dist dist-lha

dist: all
	mkdir -p $(DIST_STAGE)/content
	cp -f $(CHIP_TARGET)                          $(DIST_STAGE)/content/virtiogpu.chip
	cp -f installer/install.py                    $(DIST_STAGE)/install.py
	cp -f installer/VirtIOGPUInstallerLocale.py   $(DIST_STAGE)/VirtIOGPUInstallerLocale.py
	cp -f README.md                               $(DIST_STAGE)/README.md
	@echo "=== Staged distribution drawer ==="
	@find $(DIST_STAGE) -type f | sort

dist-lha: dist
	rm -f $(DIST_LHA)
	cd $(DIST_DIR) && lha ao5q ../VirtIOGPU.lha VirtIOGPU
	@ls -la $(DIST_LHA)
