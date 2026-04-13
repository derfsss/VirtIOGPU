#ifndef VIRTIO_CONFIG_H
#define VIRTIO_CONFIG_H

/*
 * virtio_config.h -- VirtIO GPU Device Configuration
 *
 * PCI Device IDs (VirtIO spec Appendix B):
 *   Modern (non-transitional):
 *     0x1050 = GPU
 *   Legacy (transitional):
 *     No standard legacy GPU device ID defined -- GPU is modern-only.
 *     Use 0x0000 as placeholder (will not match any device).
 */

/* Device name as seen by AmigaOS */
#define VIRTIO_DEV_NAME  "virtiogpu.device"

/* PCI Device IDs */
#define VIRTIO_PCI_DEVICE_ID_MODERN  0x1050
#define VIRTIO_PCI_DEVICE_ID_LEGACY  0x0000  /* No legacy GPU */

/*
 * VirtIO GPU has 2 virtqueues:
 *   Queue 0 = Control (2D/3D commands, display management)
 *   Queue 1 = Cursor  (cursor position/bitmap updates, fast path)
 */
#define VIRTIO_NUM_QUEUES  2
#define VIRTIO_GPU_CTRLQ   0
#define VIRTIO_GPU_CURSQ   1

/* Maximum number of units (one per physical display scanout, up to 16) */
#define VIRTIO_MAX_UNITS   16

/* Maximum virtqueues the base struct can hold */
#define VIRTIO_MAX_QUEUES  8

/*
 * VirtIO GPU Feature bits accepted during negotiation.
 *
 * Bit 0: VIRTIO_GPU_F_VIRGL      -- 3D acceleration via virglrenderer
 * Bit 1: VIRTIO_GPU_F_EDID       -- EDID monitor info retrieval
 * Bit 2: VIRTIO_GPU_F_RESOURCE_UUID -- Resource UUID assignment
 * Bit 3: VIRTIO_GPU_F_RESOURCE_BLOB -- Blob resource (host-visible memory)
 * Bit 4: VIRTIO_GPU_F_CONTEXT_INIT  -- Multi-timeline context init
 *
 * For Phase 1 (framebuffer only) we do not need VIRGL.
 * Accept EDID if available for better display info.
 */
#define VIRTIO_GPU_F_VIRGL           (1U << 0)
#define VIRTIO_GPU_F_EDID            (1U << 1)
#define VIRTIO_GPU_F_RESOURCE_UUID   (1U << 2)
#define VIRTIO_GPU_F_RESOURCE_BLOB   (1U << 3)
#define VIRTIO_GPU_F_CONTEXT_INIT    (1U << 4)

/* Accept EDID + VIRGL + RESOURCE_BLOB.
 * VIRGL is optional -- if the device doesn't advertise it, 2D-only mode works.
 * RESOURCE_BLOB is optional -- requires virtio-gpu-gl in QEMU. */
#define VIRTIO_DEVICE_FEATURES_MASK  (VIRTIO_GPU_F_EDID | VIRTIO_GPU_F_VIRGL | VIRTIO_GPU_F_RESOURCE_BLOB)

/*
 * Resident priority.
 * The GPU driver must load before Workbench graphics initialises,
 * but after PCI enumeration is complete.
 * Use -50 (slightly higher than SCSI's -60) so the display is ready
 * before disk mounts start showing icons.
 */
#define VIRTIO_RESIDENT_PRI  -50

#endif /* VIRTIO_CONFIG_H */
