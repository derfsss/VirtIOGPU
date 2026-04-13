#ifndef VIRTIO_GPU_PROTO_H
#define VIRTIO_GPU_PROTO_H

/*
 * virtio_gpu_proto.h -- VirtIO GPU Protocol Structures
 *
 * Based on the VirtIO 1.2 specification, Section 5.7 (GPU Device).
 * All multi-byte fields are little-endian (LE) as per VirtIO spec.
 *
 * On PPC (big-endian) all struct fields must be byte-swapped on access.
 * Use the GP32/GP16/GP64 macros below for every struct field read/write.
 *
 * No external dependencies -- no clib, no SDK headers required here.
 */

#include <exec/types.h>

/*
 * GPU protocol byte-swap helpers (Modern VirtIO = always little-endian).
 *
 * PPC is big-endian, VirtIO GPU wire format is little-endian.
 * Every field in every virtio_gpu_* struct must be byte-swapped on
 * both write (cpu->le) and read (le->cpu).
 *
 * GP32(v)  -- convert uint32 between CPU and LE (symmetric, use for both R and W)
 * GP16(v)  -- convert uint16 between CPU and LE
 * GP64(v)  -- convert uint64 between CPU and LE
 */
#define GP32(v)  __builtin_bswap32(v)
#define GP16(v)  __builtin_bswap16(v)
#define GP64(v)  __builtin_bswap64(v)

/* -----------------------------------------------------------------------
 * Control command types (sent on Queue 0)
 * ----------------------------------------------------------------------- */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO         0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D       0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF           0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT              0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH           0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D      0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING  0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING  0x0107
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO          0x0108
#define VIRTIO_GPU_CMD_GET_CAPSET               0x0109
#define VIRTIO_GPU_CMD_GET_EDID                 0x010A

/* 3D context commands (requires VIRTIO_GPU_F_VIRGL) */
#define VIRTIO_GPU_CMD_CTX_CREATE               0x0200
#define VIRTIO_GPU_CMD_CTX_DESTROY              0x0201
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE      0x0202
#define VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE      0x0203
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D       0x0204
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D      0x0205
#define VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D    0x0206
#define VIRTIO_GPU_CMD_SUBMIT_3D                0x0207

/* Blob resources (requires VIRTIO_GPU_F_RESOURCE_BLOB) */
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB     0x010D
#define VIRTIO_GPU_CMD_SET_SCANOUT_BLOB         0x010E
#define VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB        0x010F
#define VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB      0x0110

/* Cursor commands (sent on Queue 1) */
#define VIRTIO_GPU_CMD_UPDATE_CURSOR            0x0300
#define VIRTIO_GPU_CMD_MOVE_CURSOR              0x0301

/* -----------------------------------------------------------------------
 * Response types
 * ----------------------------------------------------------------------- */
#define VIRTIO_GPU_RESP_OK_NODATA               0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO         0x1101
#define VIRTIO_GPU_RESP_OK_CAPSET_INFO          0x1102
#define VIRTIO_GPU_RESP_OK_CAPSET               0x1103
#define VIRTIO_GPU_RESP_OK_EDID                 0x1104

#define VIRTIO_GPU_RESP_ERR_UNSPEC              0x1200
#define VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY       0x1201
#define VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID  0x1202
#define VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID 0x1203
#define VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID  0x1204
#define VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER   0x1205

/* -----------------------------------------------------------------------
 * Command header flags
 * ----------------------------------------------------------------------- */
#define VIRTIO_GPU_FLAG_FENCE               (1 << 0)
#define VIRTIO_GPU_FLAG_INFO_RING_IDX       (1 << 1)

/* -----------------------------------------------------------------------
 * Pixel formats
 * Only 32-bit formats guaranteed on QEMU virtiogpu.
 * VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM is the safest default.
 * ----------------------------------------------------------------------- */
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM    1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM    2
#define VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM    3
#define VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM    4
#define VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM    67
#define VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM    68
#define VIRTIO_GPU_FORMAT_A8B8G8G8_UNORM    121
#define VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM    134

/* Maximum number of independent display outputs */
#define VIRTIO_GPU_MAX_SCANOUTS             16

/* -----------------------------------------------------------------------
 * Control header -- prefix for every command and response
 * ----------------------------------------------------------------------- */
struct virtio_gpu_ctrl_hdr {
    uint32 type;        /* VIRTIO_GPU_CMD_* or VIRTIO_GPU_RESP_* */
    uint32 flags;       /* VIRTIO_GPU_FLAG_* */
    uint64 fence_id;    /* For fenced operations */
    uint32 ctx_id;      /* Context ID; 0 for 2D */
    uint8  ring_idx;    /* Ring index (VIRTIO_GPU_FLAG_INFO_RING_IDX) */
    uint8  padding[3];
};

/* -----------------------------------------------------------------------
 * Geometry types
 * ----------------------------------------------------------------------- */
struct virtio_gpu_rect {
    uint32 x;
    uint32 y;
    uint32 width;
    uint32 height;
};

/* -----------------------------------------------------------------------
 * GET_DISPLAY_INFO
 * ----------------------------------------------------------------------- */
struct virtio_gpu_display_one {
    struct virtio_gpu_rect r;    /* Current display rectangle */
    uint32 enabled;              /* 1 = display enabled */
    uint32 flags;                /* Reserved */
};

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one pmodes[VIRTIO_GPU_MAX_SCANOUTS];
};

/* -----------------------------------------------------------------------
 * RESOURCE_CREATE_2D
 * ----------------------------------------------------------------------- */
struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 resource_id;          /* Non-zero unique ID chosen by driver */
    uint32 format;               /* VIRTIO_GPU_FORMAT_* */
    uint32 width;
    uint32 height;
};

/* -----------------------------------------------------------------------
 * RESOURCE_UNREF
 * ----------------------------------------------------------------------- */
struct virtio_gpu_resource_unref {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 resource_id;
    uint32 padding;
};

/* -----------------------------------------------------------------------
 * SET_SCANOUT
 * ----------------------------------------------------------------------- */
struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;    /* Display rectangle on this scanout */
    uint32 scanout_id;           /* 0 .. VIRTIO_GPU_MAX_SCANOUTS-1 */
    uint32 resource_id;          /* 0 = disable scanout */
};

/* -----------------------------------------------------------------------
 * RESOURCE_FLUSH -- push pixels from host resource to display
 * ----------------------------------------------------------------------- */
struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32 resource_id;
    uint32 padding;
};

/* -----------------------------------------------------------------------
 * TRANSFER_TO_HOST_2D -- copy guest memory to resource on host
 * ----------------------------------------------------------------------- */
struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;    /* Region within resource */
    uint64 offset;               /* Byte offset in backing store */
    uint32 resource_id;
    uint32 padding;
};

/* -----------------------------------------------------------------------
 * RESOURCE_ATTACH_BACKING -- bind guest physical memory pages to resource
 * ----------------------------------------------------------------------- */
struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 resource_id;
    uint32 nr_entries;           /* Number of mem_entry structs that follow */
};

struct virtio_gpu_mem_entry {
    uint64 addr;                 /* Guest physical address */
    uint32 length;               /* Length in bytes */
    uint32 padding;
};

/* -----------------------------------------------------------------------
 * RESOURCE_DETACH_BACKING
 * ----------------------------------------------------------------------- */
struct virtio_gpu_resource_detach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 resource_id;
    uint32 padding;
};

/* -----------------------------------------------------------------------
 * GET_EDID (VIRTIO_GPU_F_EDID)
 * ----------------------------------------------------------------------- */
struct virtio_gpu_get_edid {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 scanout;
    uint32 padding;
};

struct virtio_gpu_resp_edid {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 size;
    uint32 padding;
    uint8  edid[1024];
};

/* -----------------------------------------------------------------------
 * Cursor commands (Queue 1 -- fast path, no blocking)
 * ----------------------------------------------------------------------- */
struct virtio_gpu_cursor_pos {
    uint32 scanout_id;
    uint32 x;
    uint32 y;
    uint32 padding;
};

struct virtio_gpu_update_cursor {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_cursor_pos pos;
    uint32 resource_id;  /* UPDATE: ID of 64x64 ARGB cursor bitmap resource */
    uint32 hot_x;        /* UPDATE: hotspot X */
    uint32 hot_y;        /* UPDATE: hotspot Y */
    uint32 padding;
};

/* -----------------------------------------------------------------------
 * Device configuration space (read via DEVICE_CFG MMIO region)
 * ----------------------------------------------------------------------- */
struct virtio_gpu_config {
    uint32 events_read;     /* Event register (read-only) */
    uint32 events_clear;    /* Write a mask to clear events */
    uint32 num_scanouts;    /* Number of scanout outputs */
    uint32 num_capsets;     /* Number of capability sets (VIRGL) */
};

/* Event bits in events_read / events_clear */
#define VIRTIO_GPU_EVENT_DISPLAY  (1 << 0)  /* Display configuration changed */

/* Capset IDs (for GET_CAPSET_INFO / GET_CAPSET) */
#define VIRTIO_GPU_CAPSET_VIRGL   1
#define VIRTIO_GPU_CAPSET_VIRGL2  2

/* -----------------------------------------------------------------------
 * GET_CAPSET_INFO -- query capability set metadata
 * ----------------------------------------------------------------------- */
struct virtio_gpu_get_capset_info {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 capset_index;    /* 0 .. num_capsets-1 */
    uint32 padding;
};

struct virtio_gpu_resp_capset_info {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 capset_id;           /* VIRTIO_GPU_CAPSET_* */
    uint32 capset_max_version;
    uint32 capset_max_size;     /* max bytes for the capset blob */
    uint32 padding;
};

/* -----------------------------------------------------------------------
 * GET_CAPSET -- retrieve actual capability set data
 * ----------------------------------------------------------------------- */
struct virtio_gpu_get_capset {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 capset_id;
    uint32 capset_version;
};

/* Response: hdr + variable-length capset data.
 * We define a fixed max buffer for simplicity. */
#define VIRTIO_GPU_MAX_CAPSET_SIZE  2048

struct virtio_gpu_resp_capset {
    struct virtio_gpu_ctrl_hdr hdr;
    uint8  capset_data[VIRTIO_GPU_MAX_CAPSET_SIZE];
};

/* -----------------------------------------------------------------------
 * CTX_CREATE / CTX_DESTROY -- 3D context lifecycle
 * ----------------------------------------------------------------------- */
struct virtio_gpu_ctx_create {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 nlen;            /* length of debug_name (excluding NUL) */
    uint32 context_init;    /* capset_id in low 8 bits (if F_CONTEXT_INIT) */
    char   debug_name[64];
};

struct virtio_gpu_ctx_destroy {
    struct virtio_gpu_ctrl_hdr hdr;
};

/* -----------------------------------------------------------------------
 * CTX_ATTACH_RESOURCE / CTX_DETACH_RESOURCE
 * ----------------------------------------------------------------------- */
struct virtio_gpu_ctx_resource {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 resource_id;
    uint32 padding;
};

/* -----------------------------------------------------------------------
 * RESOURCE_CREATE_3D -- create 3D-capable resource
 * ----------------------------------------------------------------------- */
struct virtio_gpu_resource_create_3d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 resource_id;
    uint32 target;          /* PIPE_TEXTURE_2D etc. */
    uint32 format;          /* PIPE_FORMAT_* */
    uint32 bind;            /* PIPE_BIND_* */
    uint32 width;
    uint32 height;
    uint32 depth;
    uint32 array_size;
    uint32 last_level;      /* mipmap levels - 1 */
    uint32 nr_samples;
    uint32 flags;
    uint32 padding;
};

/* -----------------------------------------------------------------------
 * TRANSFER_TO_HOST_3D / TRANSFER_FROM_HOST_3D
 * ----------------------------------------------------------------------- */
struct virtio_gpu_box {
    uint32 x, y, z;
    uint32 w, h, d;
};

struct virtio_gpu_transfer_host_3d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_box box;
    uint64 offset;
    uint32 resource_id;
    uint32 level;
    uint32 stride;
    uint32 layer_stride;
};

/* -----------------------------------------------------------------------
 * SUBMIT_3D -- submit Virgl command stream
 *
 * The command data follows immediately after this header in the virtqueue
 * scatter-gather list (separate OUT entry).
 * ----------------------------------------------------------------------- */
struct virtio_gpu_cmd_submit {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 size;            /* size of command buffer in bytes */
    uint32 padding;
};

/* -----------------------------------------------------------------------
 * RESOURCE_CREATE_BLOB -- blob resource (zero-copy host-guest sharing)
 * Requires VIRTIO_GPU_F_RESOURCE_BLOB feature.
 * ----------------------------------------------------------------------- */

/* Blob memory types */
#define VIRTIO_GPU_BLOB_MEM_GUEST           0x0001
#define VIRTIO_GPU_BLOB_MEM_HOST3D          0x0002
#define VIRTIO_GPU_BLOB_MEM_HOST3D_GUEST    0x0003

/* Blob flags */
#define VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE   (1 << 0)
#define VIRTIO_GPU_BLOB_FLAG_USE_SHAREABLE  (1 << 1)
#define VIRTIO_GPU_BLOB_FLAG_USE_CROSS_DEVICE (1 << 2)

struct virtio_gpu_resource_create_blob {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 resource_id;
    uint32 blob_mem;        /* VIRTIO_GPU_BLOB_MEM_* */
    uint32 blob_flags;      /* VIRTIO_GPU_BLOB_FLAG_* */
    uint32 nr_entries;      /* number of mem_entry structs following */
    uint64 blob_id;         /* host-side blob identifier */
    uint64 size;            /* total blob size in bytes */
};

struct virtio_gpu_resource_map_blob {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 resource_id;
    uint32 padding;
    uint64 offset;          /* offset into mapped region */
};

struct virtio_gpu_resp_map_info {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32 map_info;        /* cache flags for mapped region */
    uint32 padding;
};

#define VIRTIO_GPU_RESP_OK_MAP_INFO         0x1106

#endif /* VIRTIO_GPU_PROTO_H */
