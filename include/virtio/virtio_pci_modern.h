#ifndef VIRTIO_PCI_MODERN_H
#define VIRTIO_PCI_MODERN_H

#include <expansion/pci.h>
#include <interfaces/expansion.h>

/*
 * virtio_pci_modern.h -- VirtIO 1.0 Modern PCI Transport Definitions
 *
 * Modern VirtIO devices advertise their configuration regions via vendor-
 * specific PCI capabilities (type 0x09).  Each capability describes one
 * config region (COMMON, NOTIFY, ISR, or DEVICE) mapped within a BAR.
 *
 * Reference: VirtIO v1.0 spec, Section 4.1.4
 */

/* VirtIO vendor-specific PCI capability cfg_type values */
#define VIRTIO_PCI_CAP_COMMON_CFG  1  /* Common configuration */
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2  /* Notifications */
#define VIRTIO_PCI_CAP_ISR_CFG     3  /* ISR status */
#define VIRTIO_PCI_CAP_DEVICE_CFG  4  /* Device-specific configuration */
#define VIRTIO_PCI_CAP_PCI_CFG     5  /* PCI configuration access */

/*
 * Byte offsets within a VirtIO vendor-specific PCI capability structure.
 *
 * Capability layout (from CapOffset):
 *   +0  cap_vndr   (uint8)  = 0x09 (vendor-specific)
 *   +1  cap_next   (uint8)  next cap offset
 *   +2  cap_len    (uint8)  length of this cap structure
 *   +3  cfg_type   (uint8)  which config region (VIRTIO_PCI_CAP_*)
 *   +4  bar        (uint8)  which BAR contains the region
 *   +5  padding[3] (uint8)
 *   +8  offset     (uint32) byte offset within the BAR
 *  +12  length     (uint32) length of the region in bytes
 *
 * For NOTIFY_CFG only:
 *  +16  notify_off_multiplier (uint32)
 *       Per-queue notify address = notify_cfg_base + queue_notify_off * multiplier
 *       If multiplier == 0, all queues use the same address (notify_cfg_base).
 */
#define VIRTIO_CAP_OFF_CFG_TYPE    3
#define VIRTIO_CAP_OFF_BAR         4
#define VIRTIO_CAP_OFF_OFFSET      8
#define VIRTIO_CAP_OFF_LENGTH     12
#define VIRTIO_CAP_OFF_NOTIFY_MULT 16  /* NOTIFY_CFG only */

/*
 * PCI_CFG capability access helpers (VirtIO spec 4.1.4.7).
 *
 * Used on platforms where direct MMIO access doesn't work (AmigaOne / Articia S).
 * Accesses VirtIO registers by writing bar/offset/length into the PCI_CFG
 * capability structure via PCI config space, then reading/writing the data field.
 * All config space accesses go via the PCI bridge and work everywhere.
 *
 * cap_off = libBase->pci_cfg_cap_offset  (config space offset of the cap)
 * bar     = libBase->pci_cfg_bar         (which BAR the target register is in)
 * reg_off = offset within that BAR (e.g. VIRTIO_PCI_COMMON_STATUS)
 *
 * Layout within the PCI_CFG cap:
 *   cap_off +  4  uint8  bar
 *   cap_off +  8  uint32 offset  (BAR-relative byte offset of register)
 *   cap_off + 12  uint32 length  (access width: 1, 2, or 4)
 *   cap_off + 16  uint32 data    (read/write register value, width-masked)
 */
#define VIRTIO_CAP_OFF_PCI_CFG_DATA  16

static inline void cfg_setup(struct PCIDevice *d, uint8 cap_off, uint8 bar,
                              uint32 reg_off, uint32 len)
{
    d->WriteConfigByte(cap_off + VIRTIO_CAP_OFF_BAR,    bar);
    d->WriteConfigLong(cap_off + VIRTIO_CAP_OFF_OFFSET, reg_off);
    d->WriteConfigLong(cap_off + VIRTIO_CAP_OFF_LENGTH, len);
}

static inline uint8 cfg_r8(struct PCIDevice *d, uint8 cap_off, uint8 bar, uint32 reg_off)
{
    cfg_setup(d, cap_off, bar, reg_off, 1);
    return (uint8)d->ReadConfigLong(cap_off + VIRTIO_CAP_OFF_PCI_CFG_DATA);
}

static inline void cfg_w8(struct PCIDevice *d, uint8 cap_off, uint8 bar, uint32 reg_off, uint8 v)
{
    cfg_setup(d, cap_off, bar, reg_off, 1);
    d->WriteConfigLong(cap_off + VIRTIO_CAP_OFF_PCI_CFG_DATA, (uint32)v);
}

static inline uint16 cfg_r16(struct PCIDevice *d, uint8 cap_off, uint8 bar, uint32 reg_off)
{
    cfg_setup(d, cap_off, bar, reg_off, 2);
    return (uint16)d->ReadConfigLong(cap_off + VIRTIO_CAP_OFF_PCI_CFG_DATA);
}

static inline void cfg_w16(struct PCIDevice *d, uint8 cap_off, uint8 bar, uint32 reg_off, uint16 v)
{
    cfg_setup(d, cap_off, bar, reg_off, 2);
    d->WriteConfigLong(cap_off + VIRTIO_CAP_OFF_PCI_CFG_DATA, (uint32)v);
}

static inline uint32 cfg_r32(struct PCIDevice *d, uint8 cap_off, uint8 bar, uint32 reg_off)
{
    cfg_setup(d, cap_off, bar, reg_off, 4);
    return d->ReadConfigLong(cap_off + VIRTIO_CAP_OFF_PCI_CFG_DATA);
}

static inline void cfg_w32(struct PCIDevice *d, uint8 cap_off, uint8 bar, uint32 reg_off, uint32 v)
{
    cfg_setup(d, cap_off, bar, reg_off, 4);
    d->WriteConfigLong(cap_off + VIRTIO_CAP_OFF_PCI_CFG_DATA, v);
}

/*
 * MMIO access helpers for Modern VirtIO Common Configuration.
 *
 * PCI accessor methods (InByte/InLong etc.) do NOT work for MMIO BAR
 * addresses on either AmigaOne (Articia S) or Pegasos2 (MV64361).
 * Direct CPU load/store via stwbrx/lwbrx (PPC byte-reversed) is the
 * only method that works on Pegasos2.
 *
 * stwbrx/lwbrx perform atomic LE<->BE conversion in hardware.
 * stb/lbz are used for 8-bit registers (STATUS, config_generation).
 * mbar after writes ensures the PCI bridge sees the store before the
 * next instruction.  eieio is insufficient on QEMU (TCG doesn't flush
 * MMIO stores with eieio alone).
 *
 * The addr parameter is the physical BAR address (e.g. 0x84200000 + offset).
 * On Pegasos2 the MV64361 transparently maps PCI BAR physical addresses
 * into the CPU address space -- direct pointer dereference works.
 *
 * The pciDev parameter is accepted but unused -- kept for call-site compat.
 */

static inline uint8 mmio_r8(struct PCIDevice *d, uint32 addr)
{
    (void)d;
    volatile uint8 *a = (volatile uint8 *)addr;
    uint8 r;
    __asm__ volatile("lbz %0,0(%1)" : "=r"(r) : "r"(a) : "memory");
    return r;
}

static inline void mmio_w8(struct PCIDevice *d, uint32 addr, uint8 v)
{
    (void)d;
    volatile uint8 *a = (volatile uint8 *)addr;
    __asm__ volatile("stb %1,0(%0); mbar" : : "r"(a), "r"(v) : "memory");
}

static inline uint16 mmio_r16(struct PCIDevice *d, uint32 addr)
{
    (void)d;
    volatile uint16 *a = (volatile uint16 *)addr;
    uint16 r;
    __asm__ volatile("lhbrx %0,0,%1" : "=r"(r) : "r"(a) : "memory");
    return r;
}

static inline void mmio_w16(struct PCIDevice *d, uint32 addr, uint16 v)
{
    (void)d;
    volatile uint16 *a = (volatile uint16 *)addr;
    __asm__ volatile("sthbrx %1,0,%0; mbar" : : "r"(a), "r"(v) : "memory");
}

static inline uint32 mmio_r32(struct PCIDevice *d, uint32 addr)
{
    (void)d;
    volatile uint32 *a = (volatile uint32 *)addr;
    uint32 r;
    __asm__ volatile("lwbrx %0,0,%1" : "=r"(r) : "r"(a) : "memory");
    return r;
}

static inline void mmio_w32(struct PCIDevice *d, uint32 addr, uint32 v)
{
    (void)d;
    volatile uint32 *a = (volatile uint32 *)addr;
    __asm__ volatile("stwbrx %1,0,%0; mbar" : : "r"(a), "r"(v) : "memory");
}

/*
 * Modern VirtIO Common Configuration register offsets (Section 4.1.4.3).
 * All registers are little-endian; use mmio_r16/mmio_r32/mmio_w16/mmio_w32 helpers above.
 */
#define VIRTIO_PCI_COMMON_DFSELECT  0x00  /* uint32 rw: device feature word select */
#define VIRTIO_PCI_COMMON_DF        0x04  /* uint32 r:  device feature bits */
#define VIRTIO_PCI_COMMON_DFSELECTG 0x08  /* uint32 rw: driver feature word select (guest) */
#define VIRTIO_PCI_COMMON_DFG       0x0C  /* uint32 rw: driver feature bits (guest) */
#define VIRTIO_PCI_COMMON_MSIX_CFG  0x10  /* uint16 rw: config vector (MSI-X) */
#define VIRTIO_PCI_COMMON_NUMQ      0x12  /* uint16 r:  number of virtqueues */
#define VIRTIO_PCI_COMMON_STATUS    0x14  /* uint8  rw: device status */
#define VIRTIO_PCI_COMMON_CFGGEN    0x15  /* uint8  r:  config generation */
#define VIRTIO_PCI_COMMON_Q_SELECT  0x16  /* uint16 rw: queue select */
#define VIRTIO_PCI_COMMON_Q_SIZE    0x18  /* uint16 rw: queue size */
#define VIRTIO_PCI_COMMON_Q_MSIX    0x1A  /* uint16 rw: queue MSI-X vector */
#define VIRTIO_PCI_COMMON_Q_ENABLE  0x1C  /* uint16 rw: queue enable */
#define VIRTIO_PCI_COMMON_Q_NOFF    0x1E  /* uint16 r:  queue notify offset */
#define VIRTIO_PCI_COMMON_Q_DESCLO  0x20  /* uint32 rw: descriptor area low 32 bits */
#define VIRTIO_PCI_COMMON_Q_DESCHI  0x24  /* uint32 rw: descriptor area high 32 bits */
#define VIRTIO_PCI_COMMON_Q_AVAILLO 0x28  /* uint32 rw: driver area (avail ring) low */
#define VIRTIO_PCI_COMMON_Q_AVAILHI 0x2C  /* uint32 rw: driver area high */
#define VIRTIO_PCI_COMMON_Q_USEDLO  0x30  /* uint32 rw: device area (used ring) low */
#define VIRTIO_PCI_COMMON_Q_USEDHI  0x34  /* uint32 rw: device area high */

#endif /* VIRTIO_PCI_MODERN_H */
