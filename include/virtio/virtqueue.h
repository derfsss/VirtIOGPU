#ifndef VIRTQUEUE_H
#define VIRTQUEUE_H

#include <exec/types.h>

/* VirtQueue Descriptor Flags */
#define VRING_DESC_F_NEXT     1
#define VRING_DESC_F_WRITE    2
#define VRING_DESC_F_INDIRECT 4

/*
 * Indirect descriptor table entry (VIRTIO_F_INDIRECT_DESC).
 * Same layout as vring_desc but used inside the indirect table buffer;
 * the 'next' field is unused -- the table is a flat array, not a chain.
 */
struct vring_indirect_desc
{
    uint64 addr;
    uint32 len;
    uint16 flags;
    uint16 next; /* unused */
} __attribute__((packed));

/* VirtQueue Available Ring Flags */
#define VRING_AVAIL_F_NO_INTERRUPT 1

/* Maximum scatter-gather entries per VirtIO request.
 * 240KB at 4KB pages = 60 data entries + req + resp = 62 max.
 * 64 provides safe headroom, allowing ~240KB transfers without chaining. */
#define MAX_SG_ENTRIES 64

/* vring_desc - 16 bytes */
struct vring_desc
{
    uint64 addr;
    uint32 len;
    uint16 flags;
    uint16 next;
} __attribute__((packed));

/* vring_avail */
struct vring_avail
{
    uint16 flags;
    uint16 idx;
    uint16 ring[];
} __attribute__((packed));

/* vring_used_elem - 8 bytes */
struct vring_used_elem
{
    uint32 id;
    uint32 len;
} __attribute__((packed));

/* vring_used */
struct vring_used
{
    uint16 flags;
    uint16 idx;
    struct vring_used_elem ring[];
} __attribute__((packed));

/*
 * Scatter-gather entry for AddBuf.
 * addr = physical address of buffer, len = length in bytes.
 */
struct vring_sg
{
    uint32 addr;
    uint32 len;
};

/*
 * Vring field byte-swap helpers for modern VirtIO.
 *
 * Modern VirtIO (VIRTIO_F_VERSION_1) requires all vring structure fields to
 * be little-endian, while the PPC guest is big-endian.  When vq->modern is
 * TRUE, every vring field read or written must go through these helpers.
 *
 * Legacy mode (vq->modern == FALSE): all fields are native big-endian -- the
 * helpers are no-ops.
 */
static inline uint16 vr16(BOOL modern, uint16 v)
{
    return modern ? __builtin_bswap16(v) : v;
}
static inline uint32 vr32(BOOL modern, uint32 v)
{
    return modern ? __builtin_bswap32(v) : v;
}
static inline uint64 vr64(BOOL modern, uint64 v)
{
    return modern ? __builtin_bswap64(v) : v;
}

/* VirtQueue High-Level Management Struct */
struct virtqueue
{
    uint32 index;
    uint32 num; /* Size of the rings (e.g. 256) */

    struct vring_desc *desc;
    struct vring_avail *avail;
    struct vring_used *used;

    /* Descriptor chain management */
    uint16 free_head;     /* Head of the free descriptor list */
    uint16 num_free;      /* Number of free descriptors */
    uint16 last_used_idx; /* Last used ring index we processed */

    /* Cookie storage: one per descriptor slot, for tracking IORequests */
    void **cookies;

    /* Base pointer to the raw allocation (for FreeVec) */
    void *mem_block;
    uint32 mem_size;

    /* DMA mapping for the vring (kept live; freed in VirtQueue_Free) */
    uint32 dma_phys;    /* Physical base address of the vring (desc table start) */
    uint32 dma_entries; /* Entry count returned by StartDMA (0 = not mapped) */

    /* Modern VirtIO: physical addresses of the three vring regions. */
    uint32 avail_phys;  /* Physical address of the avail (driver) ring */
    uint32 used_phys;   /* Physical address of the used (device) ring */

    /* Modern VirtIO: queue notification address.
     * Set to: notify_cfg_base + queue_notify_off * notify_off_mult. */
    uint32 notify_addr;

    /* BAR-relative offset of the notify register (for PCI_CFG cap access). */
    uint32 notify_bar_off;

    /* TRUE if this queue uses little-endian vring fields (modern VirtIO). */
    BOOL modern;

    /* VIRTIO_F_EVENT_IDX: suppress redundant kicks. */
    BOOL   use_event_idx;
    uint16 last_kick_avail_idx;

    /* VIRTIO_F_INDIRECT_DESC: entire SG chain in one heap buffer. */
    BOOL   use_indirect;
    void **indirect_tables;
};

/* Function Prototypes */
struct ExecIFace; /* forward declaration */
struct PCIDevice; /* forward declaration */

struct virtqueue *VirtQueue_Allocate(struct ExecIFace *IExec, uint32 queue_index, uint32 queue_size);
void VirtQueue_Free(struct ExecIFace *IExec, struct virtqueue *vq);

int32 VirtQueue_AddBuf(struct ExecIFace *IExec, struct virtqueue *vq, struct vring_sg *sg, uint32 out_num, uint32 in_num, void *cookie);

void *VirtQueue_GetBuf(struct ExecIFace *IExec, struct virtqueue *vq, uint32 *len_out);

#endif /* VIRTQUEUE_H */
