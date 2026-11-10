// SPDX-License-Identifier: MIT
/*
 * fifo.c - DockChannel FIFO transport (register access, interrupt, RX/TX).
 *
 * Register semantics follow the Linux dockchannel driver [DC], used as a
 * protocol reference only. Untested: never run on hardware or on Windows.
 *
 * Interrupt model. One level-triggered line (AIC 850 -> GIC INTID 850 under
 * the m1n1 hypervisor) serves the whole DockChannel IRQ block; inside it the
 * HID FIFO owns sub-IRQ bits TX and RX. Like Linux's handle_level_irq path,
 * the ISR masks the bits it saw, clears them, and defers the work; the
 * condition is re-armed by unmasking once the FIFO has been serviced. The
 * deferred work runs as the WDF interrupt work item at PASSIVE_LEVEL because
 * packet handling may create VHF devices and wait for command ACKs elsewhere.
 */
#include "driver.h"

static ULONG DcRead(PUCHAR Base, ULONG Off)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(Base + Off));
}

static VOID DcWrite(PUCHAR Base, ULONG Off, ULONG Val)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(Base + Off), Val);
}

/* The IRQ mask is shared with the ISR, so change it under the interrupt lock. */
static VOID DcIrqUpdate(PDEVICE_CONTEXT Ctx, ULONG Set, ULONG Clear)
{
    ULONG mask;

    WdfInterruptAcquireLock(Ctx->Interrupt);
    mask = DcRead(Ctx->IrqBase, DC_IRQ_MASK);
    DcWrite(Ctx->IrqBase, DC_IRQ_MASK, (mask & ~Clear) | Set);
    WdfInterruptReleaseLock(Ctx->Interrupt);
}

BOOLEAN DcEvtInterruptIsr(WDFINTERRUPT Interrupt, ULONG MessageID)
{
    PDEVICE_CONTEXT ctx = DchidGetContext(WdfInterruptGetDevice(Interrupt));
    ULONG mask, hit;

    UNREFERENCED_PARAMETER(MessageID);

    mask = DcRead(ctx->IrqBase, DC_IRQ_MASK);
    hit = DcRead(ctx->IrqBase, DC_IRQ_FLAG) & mask & DC_SUBIRQ_HID;
    if (hit == 0)
        return FALSE;

    /* Mask, then write-1-to-clear, as Linux does for level sub-IRQs [DC:285-309]. */
    DcWrite(ctx->IrqBase, DC_IRQ_MASK, mask & ~hit);
    DcWrite(ctx->IrqBase, DC_IRQ_FLAG, hit);
    InterlockedOr(&ctx->IrqPending, (LONG)hit);
    WdfInterruptQueueWorkItemForIsr(Interrupt);
    return TRUE;
}

/*
 * Called at DIRQL with the interrupt lock held. Start with every sub-IRQ
 * masked and all flags cleared, as the Linux probe does [DC:348-349]; the
 * RX bit is unmasked by DcRxStart once the device context is ready.
 * This assumes the HID FIFO is the only user of this IRQ block, which is
 * true for the J413 device tree (mtp_hid is its only child) [T:1613-1646].
 */
NTSTATUS DcEvtInterruptEnable(WDFINTERRUPT Interrupt, WDFDEVICE AssociatedDevice)
{
    PDEVICE_CONTEXT ctx = DchidGetContext(AssociatedDevice);

    UNREFERENCED_PARAMETER(Interrupt);
    DcWrite(ctx->IrqBase, DC_IRQ_MASK, 0);
    DcWrite(ctx->IrqBase, DC_IRQ_FLAG, 0xffffffffu);
    return STATUS_SUCCESS;
}

/* Same register sequence as the Linux remove path [DC:382-383]. */
NTSTATUS DcEvtInterruptDisable(WDFINTERRUPT Interrupt, WDFDEVICE AssociatedDevice)
{
    PDEVICE_CONTEXT ctx = DchidGetContext(AssociatedDevice);

    UNREFERENCED_PARAMETER(Interrupt);
    DcWrite(ctx->IrqBase, DC_IRQ_MASK, 0);
    DcWrite(ctx->IrqBase, DC_IRQ_FLAG, 0xffffffffu);
    return STATUS_SUCCESS;
}

/* Wait for Event, giving up on timeout or when the device stops. */
NTSTATUS DcWait(PDEVICE_CONTEXT Ctx, PKEVENT Event, LONG TimeoutMs)
{
    PVOID objs[2] = { Event, &Ctx->StopEvent };
    LARGE_INTEGER timeout;
    NTSTATUS st;

    timeout.QuadPart = -10000LL * TimeoutMs;
    st = KeWaitForMultipleObjects(2, objs, WaitAny, Executive, KernelMode, FALSE,
                                  &timeout, NULL);
    if (st == STATUS_WAIT_0)
        return STATUS_SUCCESS;
    if (st == STATUS_WAIT_1)
        return STATUS_DEVICE_NOT_READY;
    return STATUS_IO_TIMEOUT;
}

/*
 * Blocking send [DC:107-146]: write whole words to TX32 and the tail bytes
 * to TX8 while TX_FREE allows; when the FIFO is full, program TX_THRESH and
 * wait for the TX sub-IRQ.
 */
NTSTATUS DcFifoSend(PDEVICE_CONTEXT Ctx, const UCHAR *Buf, SIZE_T Len)
{
    while (Len > 0) {
        SIZE_T avail = DcRead(Ctx->DataBase, DC_DATA_TX_FREE);
        SIZE_T block = min(Len, avail);
        NTSTATUS st;

        if (avail == 0) {
            KeClearEvent(&Ctx->TxEvent);
            DcWrite(Ctx->CfgBase, DC_CFG_TX_THRESH, (ULONG)min(DC_FIFO_SIZE / 2, Len));
            DcIrqUpdate(Ctx, DC_SUBIRQ_TX, 0);
            st = DcWait(Ctx, &Ctx->TxEvent, DC_TX_TIMEOUT_MS);
            if (!NT_SUCCESS(st)) {
                DcIrqUpdate(Ctx, 0, DC_SUBIRQ_TX);
                DCHID_ERR("TX FIFO wait failed 0x%08x", st);
                return st;
            }
            continue;
        }

        for (; block >= 4; block -= 4, Len -= 4, Buf += 4)
            DcWrite(Ctx->DataBase, DC_DATA_TX32,
                    (ULONG)Buf[0] | ((ULONG)Buf[1] << 8) |
                    ((ULONG)Buf[2] << 16) | ((ULONG)Buf[3] << 24));
        for (; block > 0; block--, Len--, Buf++)
            WRITE_REGISTER_UCHAR(Ctx->DataBase + DC_DATA_TX8, *Buf);
    }
    return STATUS_SUCCESS;
}

static VOID DcRxConsume(PDEVICE_CONTEXT Ctx, const UCHAR *Data, SIZE_T Len)
{
    while (Len > 0) {
        size_t used = 0;
        dchid_rx_result r = dchid_rx_feed(&Ctx->Rx, Data, Len, &used);

        Data += used;
        Len -= used;
        switch (r) {
        case DCHID_RX_PACKET:
            DchidHandlePacket(Ctx, &Ctx->Rx.hdr, Ctx->Rx.body);
            break;
        case DCHID_RX_BAD_HDR_LEN:
            DCHID_ERR("bad header length %u, header dropped", Ctx->Rx.hdr.hdr_len);
            break;
        case DCHID_RX_BAD_CHECKSUM:
            DCHID_ERR("checksum mismatch (iface %u), packet dropped", Ctx->Rx.hdr.iface);
            break;
        case DCHID_RX_NEED_MORE:
            break;
        }
    }
}

/*
 * Drain what the FIFO holds [DC:149-188]: whole words from RX32, single
 * bytes from RX8 (the byte is in bits [15:8]). Bounded so one burst cannot
 * monopolise the worker; leftover data re-raises the RX sub-IRQ once armed.
 */
static VOID DcRxDrain(PDEVICE_CONTEXT Ctx)
{
    UCHAR chunk[256];
    SIZE_T budget = DC_RX_BUDGET;

    while (budget > 0) {
        SIZE_T n = DcRead(Ctx->DataBase, DC_DATA_RX_COUNT);
        SIZE_T i = 0;

        n = min(n, min(sizeof(chunk), budget));
        if (n == 0)
            break;
        for (; i + 4 <= n; i += 4) {
            ULONG w = DcRead(Ctx->DataBase, DC_DATA_RX32);

            chunk[i] = (UCHAR)w;
            chunk[i + 1] = (UCHAR)(w >> 8);
            chunk[i + 2] = (UCHAR)(w >> 16);
            chunk[i + 3] = (UCHAR)(w >> 24);
        }
        for (; i < n; i++)
            chunk[i] = (UCHAR)(DcRead(Ctx->DataBase, DC_DATA_RX8) >> 8);

        DcRxConsume(Ctx, chunk, n);
        budget -= n;
    }
}

/*
 * Arm the RX sub-IRQ for the rest of the current header or body, capped at
 * half the FIFO, matching the thresholds Linux uses [DC:158-161, DC:195-207].
 * Assumes the RX condition is level-sensitive (data already above the
 * threshold re-raises the flag after unmask); Linux relies on the same.
 */
static VOID DcRxArm(PDEVICE_CONTEXT Ctx)
{
    SIZE_T want = dchid_rx_wanted(&Ctx->Rx);
    BOOLEAN stopping;

    WdfSpinLockAcquire(Ctx->Lock);
    stopping = Ctx->Stopping;
    WdfSpinLockRelease(Ctx->Lock);
    if (stopping)
        return;

    DcWrite(Ctx->CfgBase, DC_CFG_RX_THRESH, (ULONG)min(want, DC_FIFO_SIZE / 2));
    DcIrqUpdate(Ctx, DC_SUBIRQ_RX, 0);
}

VOID DcRxStart(PDEVICE_CONTEXT Ctx)
{
    dchid_rx_reset(&Ctx->Rx);
    DcRxArm(Ctx);
}

VOID DcEvtInterruptWorkItem(WDFINTERRUPT Interrupt, WDFOBJECT AssociatedObject)
{
    PDEVICE_CONTEXT ctx = DchidGetContext(AssociatedObject);
    ULONG pending = (ULONG)InterlockedExchange(&ctx->IrqPending, 0);

    UNREFERENCED_PARAMETER(Interrupt);

    if (pending & DC_SUBIRQ_TX)
        KeSetEvent(&ctx->TxEvent, IO_NO_INCREMENT, FALSE);
    if (pending & DC_SUBIRQ_RX) {
        DcRxDrain(ctx);
        DcRxArm(ctx);
    }
}
