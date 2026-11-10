// SPDX-License-Identifier: MIT
/*
 * dchid.c - DockChannel HID command/event layer and VHF devices.
 *
 * Bring-up sequence (all initiated by the device; the host only reacts),
 * as described by the reference [H:779-862, H:725-777]:
 *   1. MTP sends one or more INIT events per interface on the comm
 *      interface, carrying the interface name, its HID report descriptor
 *      and an optional GPIO request.
 *   2. After the last INIT packet of "stm", the host enables it
 *      (ENABLE_INTERFACE). Other interfaces are deferred until STM is ready.
 *   3. STM sends READY; the host reads its identity (feature 0x10) and then
 *      enables every deferred interface. Interfaces that need firmware
 *      (multi-touch) get it at this point (stubbed, see platform.c).
 *   4. Each interface sends READY; input reports follow.
 *
 * Windows mapping: a VHF device is created per interface when it becomes
 * READY (VHF has no "open" callback, which is where Linux waits for READY
 * [H:554-572]). Input reports are forwarded with VhfReadReportSubmit.
 *
 * Threads:
 *   - interrupt work item (fifo.c): parses packets, completes command ACKs,
 *     forwards input reports, queues comm events;
 *   - job worker (this file): handles comm events and VHF feature/output
 *     requests. It is the only sender of commands, which keeps one command
 *     outstanding at a time as the reference does [H:321-356].
 *
 * Untested: never run on hardware or on Windows.
 */
#include "platform.h"
#include <ntstrsafe.h>

typedef enum _DCHID_JOB_KIND {
    DchidJobCommEvent,
    DchidJobVhfGetFeature,
    DchidJobVhfSetFeature,
    DchidJobVhfWriteReport
} DCHID_JOB_KIND;

typedef struct _DCHID_JOB {
    LIST_ENTRY         Link;
    DCHID_JOB_KIND     Kind;
    /* VHF requests */
    PDCHID_IFACE       Iface;
    VHFOPERATIONHANDLE Op;
    PHID_XFER_PACKET   Packet;
    /* comm events: copy of the payload */
    SIZE_T             Len;
    UCHAR              Data[];
} DCHID_JOB, *PDCHID_JOB;

static EVT_VHF_ASYNC_OPERATION DchidEvtVhfGetFeature;
static EVT_VHF_ASYNC_OPERATION DchidEvtVhfSetFeature;
static EVT_VHF_ASYNC_OPERATION DchidEvtVhfWriteReport;

static BOOLEAN DchidIsStm(const DCHID_IFACE *Iface)
{
    return strcmp(Iface->Name, "stm") == 0;
}

/* ---- Job queue ---------------------------------------------------------- */

/*
 * Enqueue under the lock so that once DchidStop has set Stopping no new
 * work item run can be scheduled behind its flush.
 */
static BOOLEAN DchidQueueJob(PDEVICE_CONTEXT Ctx, PDCHID_JOB Job)
{
    BOOLEAN queued = FALSE;

    WdfSpinLockAcquire(Ctx->Lock);
    if (!Ctx->Stopping) {
        InsertTailList(&Ctx->Jobs, &Job->Link);
        WdfWorkItemEnqueue(Ctx->JobWorker);
        queued = TRUE;
    }
    WdfSpinLockRelease(Ctx->Lock);
    return queued;
}

static PDCHID_JOB DchidDequeueJob(PDEVICE_CONTEXT Ctx)
{
    PDCHID_JOB job = NULL;

    WdfSpinLockAcquire(Ctx->Lock);
    if (!IsListEmpty(&Ctx->Jobs))
        job = CONTAINING_RECORD(RemoveHeadList(&Ctx->Jobs), DCHID_JOB, Link);
    WdfSpinLockRelease(Ctx->Lock);
    return job;
}

/* Every VHF operation must be completed exactly once, also on teardown. */
static VOID DchidFreeJob(PDCHID_JOB Job, NTSTATUS VhfStatus)
{
    if (Job->Kind != DchidJobCommEvent)
        VhfAsyncOperationComplete(Job->Op, VhfStatus);
    ExFreePoolWithTag(Job, DCHID_POOL_TAG);
}

/* ---- Commands ----------------------------------------------------------- */

/*
 * Send one command to Iface and wait for its ACK [H:315-357]. Data[0] is
 * the report id the ACK must echo. On success *RespLen (if given) is the
 * number of response bytes after the echoed report id copied to Resp.
 * Only the job worker calls this.
 */
static NTSTATUS DchidCommand(PDEVICE_CONTEXT Ctx, PDCHID_IFACE Iface, UCHAR Group,
                             UCHAR Req, const UCHAR *Data, SIZE_T Len,
                             PUCHAR Resp, SIZE_T RespCap, SIZE_T *RespLen)
{
    SIZE_T frameLen = dchid_frame_size(Len);
    UCHAR flags = dchid_flags(Group, Req);
    PUCHAR frame;
    NTSTATUS st;
    BOOLEAN done;
    ULONG retcode;
    SIZE_T got;

    if (Len == 0)
        return STATUS_INVALID_PARAMETER;
    frame = ExAllocatePool2(POOL_FLAG_NON_PAGED, frameLen, DCHID_POOL_TAG);
    if (frame == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    if (dchid_build_frame(frame, frameLen, Iface->Index, Iface->TxSeq, flags, Data, Len) == 0) {
        ExFreePoolWithTag(frame, DCHID_POOL_TAG);
        return STATUS_INVALID_BUFFER_SIZE;
    }

    KeClearEvent(&Ctx->AckEvent);
    WdfSpinLockAcquire(Ctx->Lock);
    if (Ctx->Stopping) {
        WdfSpinLockRelease(Ctx->Lock);
        ExFreePoolWithTag(frame, DCHID_POOL_TAG);
        return STATUS_DEVICE_NOT_READY;
    }
    Ctx->Cmd.iface = Iface->Index;
    Ctx->Cmd.seq = Iface->TxSeq;
    Ctx->Cmd.flags = flags;
    Ctx->Cmd.report_id = Data[0];
    Ctx->CmdResp = Resp;
    Ctx->CmdRespCap = Resp ? RespCap : 0;
    Ctx->CmdRespLen = 0;
    Ctx->CmdRetcode = 0;
    Ctx->CmdDone = FALSE;
    Ctx->CmdActive = TRUE;
    WdfSpinLockRelease(Ctx->Lock);

    st = DcFifoSend(Ctx, frame, frameLen);
    if (NT_SUCCESS(st))
        st = DcWait(Ctx, &Ctx->AckEvent, DCHID_CMD_TIMEOUT_MS);

    WdfSpinLockAcquire(Ctx->Lock);
    done = Ctx->CmdDone;
    retcode = Ctx->CmdRetcode;
    got = Ctx->CmdRespLen;
    Ctx->CmdActive = FALSE;
    Ctx->CmdResp = NULL;
    WdfSpinLockRelease(Ctx->Lock);

    /* The reference advances the sequence after every command, failed or not. */
    Iface->TxSeq++;
    ExFreePoolWithTag(frame, DCHID_POOL_TAG);

    if (!done) {
        DCHID_ERR("command 0x%02x to iface %u (%s) failed: 0x%08x",
                  Data[0], Iface->Index, Iface->Name, NT_SUCCESS(st) ? STATUS_IO_TIMEOUT : st);
        return NT_SUCCESS(st) ? STATUS_IO_TIMEOUT : st;
    }
    if (retcode != 0) {
        DCHID_ERR("command 0x%02x to iface %u (%s) returned 0x%08lx",
                  Data[0], Iface->Index, Iface->Name, retcode);
        return STATUS_IO_DEVICE_ERROR;
    }
    if (RespLen)
        *RespLen = got;
    return STATUS_SUCCESS;
}

static NTSTATUS DchidCommCommand(PDEVICE_CONTEXT Ctx, const UCHAR *Data, SIZE_T Len)
{
    return DchidCommand(Ctx, &Ctx->Ifaces[DCHID_IFACE_COMM], DCHID_TYPE_FEATURE,
                        DCHID_REQ_SET_REPORT, Data, Len, NULL, 0, NULL);
}

/* ---- Receive path (interrupt work item) ---------------------------------- */

static VOID DchidHandleAck(PDEVICE_CONTEXT Ctx, const dchid_hdr *Hdr, const dchid_msg *Msg)
{
    dchid_ack_result r = DCHID_ACK_MATCH;
    BOOLEAN active;

    WdfSpinLockAcquire(Ctx->Lock);
    active = Ctx->CmdActive && !Ctx->CmdDone;
    if (active) {
        r = dchid_match_ack(&Ctx->Cmd, Hdr, Msg);
        if (r == DCHID_ACK_MATCH) {
            SIZE_T n = min(Msg->payload_len - 1, Ctx->CmdRespCap);

            if (n)
                RtlCopyMemory(Ctx->CmdResp, Msg->payload + 1, n);
            Ctx->CmdRespLen = n;
            Ctx->CmdRetcode = Msg->sub.retcode;
            Ctx->CmdDone = TRUE;
            KeSetEvent(&Ctx->AckEvent, IO_NO_INCREMENT, FALSE);
        }
    }
    WdfSpinLockRelease(Ctx->Lock);

    if (!active)
        DCHID_WARN("unsolicited ACK on iface %u seq %u", Hdr->iface, Hdr->seq);
    else if (r != DCHID_ACK_MATCH)
        DCHID_WARN("ACK on iface %u seq %u rejected (reason %d)", Hdr->iface, Hdr->seq, (int)r);
}

static VOID DchidQueueCommEvent(PDEVICE_CONTEXT Ctx, const dchid_msg *Msg)
{
    PDCHID_JOB job;

    if (Msg->payload_len == 0)
        return;
    job = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*job) + Msg->payload_len, DCHID_POOL_TAG);
    if (job == NULL) {
        DCHID_ERR("%s", "out of memory, comm event dropped");
        return;
    }
    job->Kind = DchidJobCommEvent;
    job->Len = Msg->payload_len;
    RtlCopyMemory(job->Data, Msg->payload, Msg->payload_len);
    if (!DchidQueueJob(Ctx, job))
        DchidFreeJob(job, STATUS_SUCCESS);
}

/*
 * Input reports carry the report id in byte 0 and are passed through
 * unchanged, as the reference does with hid_input_report [H:936-949].
 * Assumption (unverified): with no EvtVhfReadyForNextReadReport, VHF copies
 * the buffer before VhfReadReportSubmit returns, so pointing into the
 * receive buffer is safe.
 */
static VOID DchidForwardInput(PDEVICE_CONTEXT Ctx, PDCHID_IFACE Iface, const dchid_msg *Msg)
{
    VHFHANDLE vhf;
    HID_XFER_PACKET pkt;
    NTSTATUS st;

    WdfSpinLockAcquire(Ctx->Lock);
    vhf = Iface->Vhf;
    WdfSpinLockRelease(Ctx->Lock);
    if (vhf == NULL || Msg->payload_len == 0)
        return;

    pkt.reportBuffer = (PUCHAR)(ULONG_PTR)Msg->payload;
    pkt.reportBufferLen = (ULONG)Msg->payload_len;
    pkt.reportId = Msg->payload[0];
    st = VhfReadReportSubmit(vhf, &pkt);
    if (!NT_SUCCESS(st))
        DCHID_WARN("VhfReadReportSubmit(%s) failed 0x%08x", Iface->Name, st);
}

VOID DchidHandlePacket(PDEVICE_CONTEXT Ctx, const dchid_hdr *Hdr, const UCHAR *Body)
{
    dchid_msg msg;

    if (dchid_decode_msg(Hdr, Body, &msg) != 0) {
        DCHID_ERR("malformed packet on iface %u (length %u)", Hdr->iface, Hdr->length);
        return;
    }
    if (Hdr->iface >= DCHID_MAX_INTERFACES) {
        DCHID_ERR("packet for out-of-range iface %u", Hdr->iface);
        return;
    }

    if (Hdr->channel == DCHID_CHANNEL_CMD) {
        DchidHandleAck(Ctx, Hdr, &msg);
        return;
    }
    if (Hdr->channel != DCHID_CHANNEL_REPORT)
        DCHID_WARN("unknown channel 0x%02x, treating as report", Hdr->channel); /* as [H:1071-1074] */

    if (msg.group != DCHID_TYPE_INPUT) {
        DCHID_WARN("unexpected report type %u on iface %u", msg.group, Hdr->iface);
        return;
    }
    if (Hdr->iface == DCHID_IFACE_COMM)
        DchidQueueCommEvent(Ctx, &msg);
    else
        DchidForwardInput(Ctx, &Ctx->Ifaces[Hdr->iface], &msg);
}

/* ---- VHF devices -------------------------------------------------------- */

static VOID DchidCreateVhf(PDEVICE_CONTEXT Ctx, PDCHID_IFACE Iface)
{
    VHF_CONFIG cfg;
    VHFHANDLE vhf;
    NTSTATUS st;
    size_t idLen = 0;

    if (Iface->Vhf != NULL)
        return;
    if (Iface->HidDesc == NULL) {
        DCHID_ERR("iface %u (%s) is ready but sent no report descriptor", Iface->Index, Iface->Name);
        return;
    }

    st = RtlStringCchPrintfW(Iface->InstanceId, ARRAYSIZE(Iface->InstanceId), L"IF%02u",
                             (unsigned)Iface->Index);
    if (NT_SUCCESS(st))
        st = RtlStringCchLengthW(Iface->InstanceId, ARRAYSIZE(Iface->InstanceId), &idLen);
    if (!NT_SUCCESS(st))
        return;

    VHF_CONFIG_INIT(&cfg, WdfDeviceWdmGetDeviceObject(Ctx->Device), Iface->HidDescLen,
                    Iface->HidDesc);
    cfg.VhfClientContext = Iface;
    cfg.VendorID = Ctx->VendorId;
    cfg.ProductID = Ctx->ProductId;
    cfg.VersionNumber = Ctx->Version;
    /* Several VHF devices share VID/PID, so give each a distinct instance id. */
    cfg.InstanceID = Iface->InstanceId;
    cfg.InstanceIDLength = (USHORT)(idLen * sizeof(WCHAR));
    cfg.EvtVhfAsyncOperationGetFeature = DchidEvtVhfGetFeature;
    cfg.EvtVhfAsyncOperationSetFeature = DchidEvtVhfSetFeature;
    cfg.EvtVhfAsyncOperationWriteReport = DchidEvtVhfWriteReport;

    st = VhfCreate(&cfg, &vhf);
    if (!NT_SUCCESS(st)) {
        DCHID_ERR("VhfCreate(%s) failed 0x%08x", Iface->Name, st);
        return;
    }
    st = VhfStart(vhf);
    if (!NT_SUCCESS(st)) {
        DCHID_ERR("VhfStart(%s) failed 0x%08x", Iface->Name, st);
        VhfDelete(vhf, TRUE);
        return;
    }

    WdfSpinLockAcquire(Ctx->Lock);
    Iface->Vhf = vhf;
    WdfSpinLockRelease(Ctx->Lock);
    DCHID_INFO("VHF device for iface %u (%s) started", Iface->Index, Iface->Name);
}

static VOID DchidQueueVhfOp(PVOID ClientContext, VHFOPERATIONHANDLE Op,
                            PHID_XFER_PACKET Packet, DCHID_JOB_KIND Kind)
{
    PDCHID_IFACE iface = ClientContext;
    PDCHID_JOB job = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*job), DCHID_POOL_TAG);

    if (job == NULL) {
        VhfAsyncOperationComplete(Op, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }
    job->Kind = Kind;
    job->Iface = iface;
    job->Op = Op;
    job->Packet = Packet;
    job->Len = 0;
    if (!DchidQueueJob(iface->Dev, job))
        DchidFreeJob(job, STATUS_DEVICE_NOT_READY);
}

static VOID DchidEvtVhfGetFeature(PVOID ClientContext, VHFOPERATIONHANDLE Op,
                                  PVOID OpContext, PHID_XFER_PACKET Packet)
{
    UNREFERENCED_PARAMETER(OpContext);
    DchidQueueVhfOp(ClientContext, Op, Packet, DchidJobVhfGetFeature);
}

static VOID DchidEvtVhfSetFeature(PVOID ClientContext, VHFOPERATIONHANDLE Op,
                                  PVOID OpContext, PHID_XFER_PACKET Packet)
{
    UNREFERENCED_PARAMETER(OpContext);
    DchidQueueVhfOp(ClientContext, Op, Packet, DchidJobVhfSetFeature);
}

static VOID DchidEvtVhfWriteReport(PVOID ClientContext, VHFOPERATIONHANDLE Op,
                                   PVOID OpContext, PHID_XFER_PACKET Packet)
{
    UNREFERENCED_PARAMETER(OpContext);
    DchidQueueVhfOp(ClientContext, Op, Packet, DchidJobVhfWriteReport);
}

/*
 * GET: send the report id, response goes after it [H:609-611].
 * SET (feature or output): the reference always sends HID_OUTPUT_REPORT
 * type for SET_REPORT, whatever the requested type [H:597-600, H:612-613];
 * kept identical (see README, "Open questions").
 */
static NTSTATUS DchidRunVhfOp(PDEVICE_CONTEXT Ctx, PDCHID_JOB Job)
{
    PHID_XFER_PACKET pkt = Job->Packet;
    UCHAR rid;

    if (pkt == NULL || pkt->reportBuffer == NULL || pkt->reportBufferLen == 0)
        return STATUS_INVALID_PARAMETER;

    if (Job->Kind == DchidJobVhfGetFeature) {
        rid = pkt->reportId;
        pkt->reportBuffer[0] = rid;
        return DchidCommand(Ctx, Job->Iface, DCHID_TYPE_FEATURE, DCHID_REQ_GET_REPORT, &rid, 1,
                            pkt->reportBuffer + 1, pkt->reportBufferLen - 1, NULL);
    }
    return DchidCommand(Ctx, Job->Iface, DCHID_TYPE_OUTPUT, DCHID_REQ_SET_REPORT,
                        pkt->reportBuffer, pkt->reportBufferLen, NULL, 0, NULL);
}

/* ---- Comm events (job worker) ------------------------------------------- */

static VOID DchidEnableInterface(PDEVICE_CONTEXT Ctx, PDCHID_IFACE Iface)
{
    UCHAR msg[2];
    NTSTATUS st;

    Iface->Deferred = FALSE;
    if (Iface->Enabled)
        return;

    st = DchidCommCommand(Ctx, msg, dchid_build_enable(msg, Iface->Index));
    if (!NT_SUCCESS(st)) {
        DCHID_ERR("failed to enable iface %u (%s): 0x%08x", Iface->Index, Iface->Name, st);
        return;
    }
    Iface->Enabled = TRUE;
    DCHID_INFO("enabled iface %u (%s)", Iface->Index, Iface->Name);

    if (DchidPlatformIfaceNeedsFirmware(Iface)) {
        st = DchidPlatformLoadFirmware(Ctx, Iface);
        if (!NT_SUCCESS(st))
            DCHID_ERR("iface %s unusable without firmware (0x%08x)", Iface->Name, st);
    }
}

static VOID DchidStoreDescriptor(PDCHID_IFACE Iface, const UCHAR *Desc, SIZE_T Len)
{
    PUCHAR copy;

    if (Iface->Vhf != NULL) {
        DCHID_WARN("descriptor for already started iface %s ignored", Iface->Name);
        return;
    }
    if (Len == 0 || Len > MAXUSHORT)
        return;
    copy = ExAllocatePool2(POOL_FLAG_NON_PAGED, Len, DCHID_POOL_TAG);
    if (copy == NULL) {
        DCHID_ERR("out of memory for %s descriptor", Iface->Name);
        return;
    }
    RtlCopyMemory(copy, Desc, Len);
    if (Iface->HidDesc)
        ExFreePoolWithTag(Iface->HidDesc, DCHID_POOL_TAG);
    Iface->HidDesc = copy;
    Iface->HidDescLen = (USHORT)Len;
}

static VOID DchidOnInit(PDEVICE_CONTEXT Ctx, const UCHAR *P, SIZE_T Len)
{
    dchid_init_info info;
    PDCHID_IFACE iface;

    if (dchid_parse_init(P, Len, &info) != 0) {
        DCHID_ERR("short INIT event (%Iu bytes)", Len);
        return;
    }
    if (info.iface == DCHID_IFACE_COMM || info.iface >= DCHID_MAX_INTERFACES) {
        DCHID_ERR("INIT for invalid iface %u", info.iface);
        return;
    }
    iface = &Ctx->Ifaces[info.iface];

    if (!iface->Present) {
        iface->Present = TRUE;
        RtlCopyMemory(iface->Name, info.name, sizeof(iface->Name));
        DCHID_INFO("new iface %u: %s", info.iface, iface->Name);
    }
    if (info.hid_desc)
        DchidStoreDescriptor(iface, info.hid_desc, info.hid_desc_len);
    if (info.has_gpio) {
        if (iface->HasGpio) {
            DCHID_ERR("iface %s requested a second GPIO; ignored", iface->Name);
        } else {
            iface->HasGpio = TRUE;
            iface->GpioId = info.gpio_id;
            RtlCopyMemory(iface->GpioName, info.gpio_name, sizeof(iface->GpioName));
            DCHID_INFO("iface %s requests GPIO #%u '%s'", iface->Name, info.gpio_id, iface->GpioName);
        }
    }
    if (info.product_name_len)
        DCHID_INFO("iface %s product '%.*s'", iface->Name,
                   (int)strnlen((const char *)info.product_name, info.product_name_len),
                   (const char *)info.product_name);
    if (info.unknown_blocks)
        DCHID_WARN("iface %s: %u unknown INIT blocks", iface->Name, info.unknown_blocks);

    if (info.more_packets)
        return;
    iface->InitDone = TRUE;

    /* STM goes first because it provides the device identity [H:857-862]. */
    if (Ctx->IdReady || DchidIsStm(iface))
        DchidEnableInterface(Ctx, iface);
    else
        iface->Deferred = TRUE;
}

static VOID DchidFetchStmId(PDEVICE_CONTEXT Ctx, PDCHID_IFACE Stm)
{
    UCHAR rid = DCHID_STM_REPORT_ID;
    UCHAR resp[DCHID_STM_ID_SIZE];
    SIZE_T got = 0;
    dchid_stm_id id;
    NTSTATUS st;

    st = DchidCommand(Ctx, Stm, DCHID_TYPE_FEATURE, DCHID_REQ_GET_REPORT, &rid, 1,
                      resp, sizeof(resp), &got);
    if (NT_SUCCESS(st) && dchid_parse_stm_id(resp, got, &id) == 0) {
        Ctx->VendorId = id.vendor_id;
        Ctx->ProductId = id.product_id;
        Ctx->Version = id.version;
        DCHID_INFO("STM id %04x:%04x version %04x keyboard type %u",
                   id.vendor_id, id.product_id, id.version, id.keyboard_type);
    } else {
        /* Same fallback as the reference: keep going with Apple's VID [H:757-762]. */
        DCHID_WARN("failed to read STM id (0x%08x, %Iu bytes)", st, got);
        Ctx->VendorId = DCHID_FALLBACK_VID;
        Ctx->ProductId = 0;
        Ctx->Version = 0;
    }
}

static VOID DchidOnReady(PDEVICE_CONTEXT Ctx, const UCHAR *P, SIZE_T Len)
{
    UCHAR index;
    PDCHID_IFACE iface;
    ULONG i;

    if (dchid_parse_ready(P, Len, &index) != 0 || index >= DCHID_MAX_INTERFACES) {
        DCHID_ERR("bad READY event (%Iu bytes)", Len);
        return;
    }
    iface = &Ctx->Ifaces[index];
    if (!iface->Present) {
        DCHID_ERR("READY for unknown iface %u", index);
        return;
    }
    iface->Ready = TRUE;
    DCHID_INFO("iface %u (%s) ready", index, iface->Name);

    if (DchidIsStm(iface) && !Ctx->IdReady) {
        DchidFetchStmId(Ctx, iface);
        Ctx->IdReady = TRUE;
        for (i = 0; i < DCHID_MAX_INTERFACES; i++) {
            if (Ctx->Ifaces[i].Deferred)
                DchidEnableInterface(Ctx, &Ctx->Ifaces[i]);
            else if (Ctx->Ifaces[i].Ready)
                DchidCreateVhf(Ctx, &Ctx->Ifaces[i]);
        }
        return;
    }
    /* Without the STM identity the VHF device would get VID/PID 0. */
    if (Ctx->IdReady)
        DchidCreateVhf(Ctx, iface);
}

/*
 * GPIO_CMD [H:865-918]: the device asks the host to drive a reset line.
 * Always ACK with the original bytes; retcode 0 only if the pulse worked.
 */
static VOID DchidOnGpio(PDEVICE_CONTEXT Ctx, const UCHAR *P, SIZE_T Len)
{
    dchid_gpio_cmd cmd;
    ULONG retcode = DCHID_GPIO_RET_ERROR;
    PUCHAR ack;
    SIZE_T ackLen;
    NTSTATUS st;

    if (dchid_parse_gpio_cmd(P, Len, &cmd) != 0)
        return;

    if (cmd.iface >= DCHID_MAX_INTERFACES || !Ctx->Ifaces[cmd.iface].Present) {
        DCHID_ERR("GPIO command for bad iface %u", cmd.iface);
    } else if (!Ctx->Ifaces[cmd.iface].HasGpio || Ctx->Ifaces[cmd.iface].GpioId != cmd.gpio) {
        DCHID_ERR("GPIO command for unknown GPIO %s#%u", Ctx->Ifaces[cmd.iface].Name, cmd.gpio);
    } else if (cmd.cmd != DCHID_GPIO_CMD_PULSE) {
        DCHID_ERR("unknown GPIO command %u", cmd.cmd);
    } else if (NT_SUCCESS(DchidPlatformGpioPulse(Ctx, &Ctx->Ifaces[cmd.iface]))) {
        retcode = 0;
    }

    ackLen = 5 + Len;
    ack = ExAllocatePool2(POOL_FLAG_NON_PAGED, ackLen, DCHID_POOL_TAG);
    if (ack == NULL)
        return;
    ackLen = dchid_build_gpio_ack(ack, ackLen, retcode, P, Len);
    st = DchidCommCommand(Ctx, ack, ackLen);
    if (!NT_SUCCESS(st))
        DCHID_ERR("failed to ACK GPIO command: 0x%08x", st);
    ExFreePoolWithTag(ack, DCHID_POOL_TAG);
}

static VOID DchidRunCommEvent(PDEVICE_CONTEXT Ctx, const UCHAR *P, SIZE_T Len)
{
    switch (P[0]) {
    case DCHID_EVENT_INIT:
        DchidOnInit(Ctx, P, Len);
        break;
    case DCHID_EVENT_READY:
        DchidOnReady(Ctx, P, Len);
        break;
    case DCHID_EVENT_GPIO_CMD:
        DchidOnGpio(Ctx, P, Len);
        break;
    default:
        DCHID_INFO("ignored comm event 0x%02x", P[0]);
        break;
    }
}

VOID DchidEvtJobWorker(WDFWORKITEM WorkItem)
{
    PDEVICE_CONTEXT ctx = DchidGetContext(WdfWorkItemGetParentObject(WorkItem));
    PDCHID_JOB job;

    while ((job = DchidDequeueJob(ctx)) != NULL) {
        NTSTATUS st = STATUS_SUCCESS;

        if (job->Kind == DchidJobCommEvent)
            DchidRunCommEvent(ctx, job->Data, job->Len);
        else
            st = DchidRunVhfOp(ctx, job);
        DchidFreeJob(job, st);
    }
}

/* ---- Start / stop / teardown -------------------------------------------- */

VOID DchidStart(PDEVICE_CONTEXT Ctx)
{
    KeClearEvent(&Ctx->StopEvent);
    WdfSpinLockAcquire(Ctx->Lock);
    Ctx->Stopping = FALSE;
    WdfSpinLockRelease(Ctx->Lock);
}

/*
 * Idempotent. After it returns no job runs, none can be queued, every
 * pending VHF operation has been completed and waits in progress were
 * aborted through StopEvent.
 */
VOID DchidStop(PDEVICE_CONTEXT Ctx)
{
    PDCHID_JOB job;

    WdfSpinLockAcquire(Ctx->Lock);
    Ctx->Stopping = TRUE;
    WdfSpinLockRelease(Ctx->Lock);
    KeSetEvent(&Ctx->StopEvent, IO_NO_INCREMENT, FALSE);
    WdfWorkItemFlush(Ctx->JobWorker);

    while ((job = DchidDequeueJob(Ctx)) != NULL)
        DchidFreeJob(job, STATUS_DEVICE_NOT_READY);
}

/*
 * Delete VHF devices and return the interface table to its initial state
 * (only the comm interface known). Called from EvtDeviceAdd on a zeroed
 * context, and from ReleaseHardware after interrupts are disconnected and
 * DchidStop, so neither the RX path nor the worker can still use a handle.
 */
VOID DchidResetInterfaces(PDEVICE_CONTEXT Ctx)
{
    ULONG i;

    for (i = 0; i < DCHID_MAX_INTERFACES; i++) {
        PDCHID_IFACE iface = &Ctx->Ifaces[i];
        VHFHANDLE vhf;

        WdfSpinLockAcquire(Ctx->Lock);
        vhf = iface->Vhf;
        iface->Vhf = NULL;
        WdfSpinLockRelease(Ctx->Lock);
        if (vhf)
            VhfDelete(vhf, TRUE);
        if (iface->HidDesc)
            ExFreePoolWithTag(iface->HidDesc, DCHID_POOL_TAG);

        RtlZeroMemory(iface, sizeof(*iface));
        iface->Dev = Ctx;
        iface->Index = (UCHAR)i;
    }
    Ctx->Ifaces[DCHID_IFACE_COMM].Present = TRUE;
    RtlCopyMemory(Ctx->Ifaces[DCHID_IFACE_COMM].Name, "comm", sizeof("comm"));
    Ctx->IdReady = FALSE;
}
