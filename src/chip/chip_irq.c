/*
 * chip_irq.c -- VirtIO GPU interrupt handler + install/remove.
 *
 * Runs on PCI INTx.  In interrupt context, reads ISR register (which
 * acknowledges the device) and signals any DoIO task waiting on a
 * queue.  DoIO uses Wait(sig_mask) instead of busy-polling when
 * irq_installed == TRUE; falls back to polling otherwise.
 *
 * Pattern from VirtualSCSIDevice/src/virtio/virtio_irq.c.
 *
 * Interrupt context restrictions:
 *  - No memory allocation
 *  - No blocking calls
 *  - No DebugPrintF
 *  - Fast path only -- work deferred to Wait-ing task
 */

#include "chip/chip_state.h"

/* VirtIO ISR register layout (single uint8 in ISR_CFG region):
 *   bit 0: queue interrupt -- at least one virtqueue has used buffers
 *   bit 1: config change   -- device configuration changed (EDID, display)
 * Read-to-clear: reading the ISR acknowledges all bits. */
#define VIRTIO_PCI_ISR_QUEUE   0x01
#define VIRTIO_PCI_ISR_CONFIG  0x02

static uint32 chip_InterruptHandler(struct ExceptionContext *ctx,
                                      struct ExecBase *SysBase,
                                      APTR is_Data)
{
    struct ChipGPUState *gs = (struct ChipGPUState *)is_Data;

    (void)ctx;
    (void)SysBase;

    /* Read ISR -- this atomically returns pending bits AND acknowledges
     * the device.  Zero means not our interrupt (shared INTx line). */
    uint8 isr;
    if (gs->use_pci_cfg) {
        /* PCI_CFG path: not expected to run (we fail init on AmigaOne),
         * but dispatch defensively. */
        isr = cfg_r8(gs->pciDevice, gs->pci_cfg_cap_off,
                     gs->isr_cfg_bar, gs->isr_cfg_off);
    } else {
        isr = mmio_read8(gs->isr_cfg_base);
    }

    if (isr == 0)
        return 0;  /* Not ours -- let next handler in chain see it */

    /* Queue interrupt: signal the DoIO caller waiting on each queue.
     * Signals are safe in interrupt context per Exec V52 semantics.
     * The sleeping DoIO task re-checks GetBuf() after Wait() returns.
     *
     * Snapshot the task/mask pair into locals before the null-check so a
     * concurrent teardown in chip_wait_ctrlq / chip_cursor_send cannot
     * produce Signal(NULL, mask) between the check and the call.  The
     * teardown clears mask=0 before task=NULL (see those functions), so
     * a locally-consistent read of either NULL task or zero mask is safe. */
    if (isr & VIRTIO_PCI_ISR_QUEUE) {
        struct ExecIFace *IExec = gs->IExec;

        struct Task *ctrl_task = gs->ctrlq_wait_task;
        uint32       ctrl_mask = gs->ctrlq_wait_mask;
        if (ctrl_task && ctrl_mask)
            IExec->Signal(ctrl_task, ctrl_mask);

        struct Task *curs_task = gs->cursorq_wait_task;
        uint32       curs_mask = gs->cursorq_wait_mask;
        if (curs_task && curs_mask)
            IExec->Signal(curs_task, curs_mask);
    }

    /* Config change: bit 1 -- we don't handle hot events in ISR, the
     * flush task's periodic poll of events_read will catch it. */

    return 1;  /* Claimed */
}

BOOL chip_irq_install(struct ChipGPUState *gs)
{
    struct ExecIFace *IExec = gs->IExec;

    gs->irq_number = gs->pciDevice->MapInterrupt();
    if (gs->irq_number == 0) {
        DCHIP("IRQ: MapInterrupt returned 0 -- using polling fallback");
        return FALSE;
    }

    gs->irq_handler.is_Node.ln_Type = NT_INTERRUPT;
    gs->irq_handler.is_Node.ln_Pri  = 0;
    gs->irq_handler.is_Node.ln_Name = (char *)"virtiogpu.chip";
    gs->irq_handler.is_Data         = (APTR)gs;
    gs->irq_handler.is_Code         = (VOID (*)())chip_InterruptHandler;

    if (!IExec->AddIntServer(gs->irq_number, &gs->irq_handler)) {
        DCHIP("IRQ: AddIntServer failed for vector %lu -- using polling fallback",
              gs->irq_number);
        return FALSE;
    }

    gs->irq_installed = TRUE;
    DCHIP("IRQ: handler installed on vector %lu", gs->irq_number);
    return TRUE;
}

void chip_irq_remove(struct ChipGPUState *gs)
{
    if (!gs->irq_installed) return;

    struct ExecIFace *IExec = gs->IExec;
    IExec->RemIntServer(gs->irq_number, &gs->irq_handler);
    gs->irq_installed = FALSE;
    DCHIP("IRQ: handler removed from vector %lu", gs->irq_number);
}
