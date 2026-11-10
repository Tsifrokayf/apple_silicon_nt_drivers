// SPDX-License-Identifier: MIT
/*
 * driver.h - AppleDockChannelHid: KMDF driver for the MacBook Air M2 (J413)
 * internal keyboard/trackpad, reached through the MTP coprocessor's
 * DockChannel FIFO. Shared declarations for all driver source files.
 *
 * Status: compiles and links only. Never loaded on Windows, never run on
 * hardware. See ../README.md.
 */
#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <vhf.h>

#include "dchid_proto.h"

#define DCHID_POOL_TAG 'dhCD'

#define DCHID_LOG(level, fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, (level), "AppleDockChannelHid: " fmt "\n", __VA_ARGS__)
#define DCHID_ERR(fmt, ...)  DCHID_LOG(DPFLTR_ERROR_LEVEL, fmt, __VA_ARGS__)
#define DCHID_WARN(fmt, ...) DCHID_LOG(DPFLTR_WARNING_LEVEL, fmt, __VA_ARGS__)
#define DCHID_INFO(fmt, ...) DCHID_LOG(DPFLTR_INFO_LEVEL, fmt, __VA_ARGS__)

/* ---- DockChannel register layout [DC:25-43] ----------------------------- */

/* IRQ block (resource 0) */
#define DC_IRQ_MASK         0x0   /* 1 = sub-IRQ enabled */
#define DC_IRQ_FLAG         0x4   /* pending, write 1 to clear */
/* FIFO config window (resource 1) */
#define DC_CFG_TX_THRESH    0x0
#define DC_CFG_RX_THRESH    0x4
/* FIFO data window (resource 2) */
#define DC_DATA_TX8         0x04
#define DC_DATA_TX32        0x10
#define DC_DATA_TX_FREE     0x14
#define DC_DATA_RX8         0x1c
#define DC_DATA_RX32        0x28
#define DC_DATA_RX_COUNT    0x2c
#define DC_DATA_MIN_LEN     0x30

/*
 * Sub-IRQ bits of the HID FIFO inside the IRQ block: the J413 device tree
 * gives the mtp_hid node interrupts <2> (tx) and <3> (rx) on the dockchannel
 * interrupt controller [T:1640-1642]. (The Linux FIFO driver also defines
 * IRQ_TX BIT(0) / IRQ_RX BIT(1) but never uses them [DC:28-29].)
 */
#define DC_SUBIRQ_TX        (1u << 2)
#define DC_SUBIRQ_RX        (1u << 3)
#define DC_SUBIRQ_HID       (DC_SUBIRQ_TX | DC_SUBIRQ_RX)

/* apple,fifo-size of mtp_hid [T:1644]. */
#define DC_FIFO_SIZE        0x800u

/* Timeouts from the reference [DC:22-23, H:23]. */
#define DC_TX_TIMEOUT_MS    1000
#define DCHID_CMD_TIMEOUT_MS 1000

/* Upper bound on bytes drained per RX work item before re-arming. */
#define DC_RX_BUDGET        0x10000u

/* Apple's USB vendor id, used when STM does not report one (as [H:761]). */
#define DCHID_FALLBACK_VID  0x05ac

/* ---- Per-interface and per-device state --------------------------------- */

typedef struct _DEVICE_CONTEXT DEVICE_CONTEXT, *PDEVICE_CONTEXT;

typedef struct _DCHID_IFACE {
    PDEVICE_CONTEXT Dev;
    UCHAR   Index;
    BOOLEAN Present;        /* an INIT packet has been seen */
    BOOLEAN InitDone;       /* last INIT packet (more_packets == 0) seen */
    BOOLEAN Deferred;       /* waiting for STM READY before enabling */
    BOOLEAN Enabled;        /* ENABLE_INTERFACE acknowledged */
    BOOLEAN Ready;          /* READY event received */
    UCHAR   TxSeq;          /* per-interface command sequence [H:283, H:350] */
    CHAR    Name[DCHID_IFACE_NAME_LEN + 1];

    PUCHAR  HidDesc;        /* report descriptor from INIT, pool allocated */
    USHORT  HidDescLen;

    BOOLEAN HasGpio;
    USHORT  GpioId;
    CHAR    GpioName[DCHID_GPIO_NAME_LEN + 1];

    VHFHANDLE Vhf;          /* written under Dev->Lock, NULL until started */
    WCHAR   InstanceId[8];
} DCHID_IFACE, *PDCHID_IFACE;

struct _DEVICE_CONTEXT {
    WDFDEVICE    Device;
    WDFINTERRUPT Interrupt;
    BOOLEAN      MtpRunning;   /* DchidPlatformMtpEnsureRunning succeeded */

    PUCHAR IrqBase;   SIZE_T IrqLen;
    PUCHAR CfgBase;   SIZE_T CfgLen;
    PUCHAR DataBase;  SIZE_T DataLen;

    /* Sub-IRQ bits latched by the ISR for the interrupt work item. */
    volatile LONG IrqPending;

    KEVENT TxEvent;     /* TX FIFO space interrupt seen */
    KEVENT AckEvent;    /* the outstanding command was acknowledged */
    KEVENT StopEvent;   /* aborts every wait when the device stops */

    /* Receive side; only the interrupt work item touches these. */
    dchid_rx Rx;
    PUCHAR   RxBody;

    /*
     * Lock protects: the outstanding command (Cmd*), the job list,
     * Stopping, and each DCHID_IFACE.Vhf pointer.
     */
    WDFSPINLOCK Lock;
    BOOLEAN       CmdActive;
    BOOLEAN       CmdDone;
    dchid_pending Cmd;
    PUCHAR        CmdResp;
    SIZE_T        CmdRespCap;
    SIZE_T        CmdRespLen;
    ULONG         CmdRetcode;

    /*
     * Serialised passive-level worker for everything that sends commands:
     * comm events and VHF feature/output requests. Invariant: only this
     * worker sends commands, so commands never overlap and TxSeq needs no lock.
     */
    WDFWORKITEM JobWorker;
    LIST_ENTRY  Jobs;
    BOOLEAN     Stopping;

    /* Identity from STM, used for every VHF device [H:754-770]. */
    BOOLEAN IdReady;
    USHORT  VendorId;
    USHORT  ProductId;
    USHORT  Version;

    DCHID_IFACE Ifaces[DCHID_MAX_INTERFACES];
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DchidGetContext)

/* driver.c */
DRIVER_INITIALIZE DriverEntry;

/* fifo.c: DockChannel FIFO transport */
EVT_WDF_INTERRUPT_ISR       DcEvtInterruptIsr;
EVT_WDF_INTERRUPT_WORKITEM  DcEvtInterruptWorkItem;
EVT_WDF_INTERRUPT_ENABLE    DcEvtInterruptEnable;
EVT_WDF_INTERRUPT_DISABLE   DcEvtInterruptDisable;
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS DcWait(_In_ PDEVICE_CONTEXT Ctx, _In_ PKEVENT Event, _In_ LONG TimeoutMs);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS DcFifoSend(_In_ PDEVICE_CONTEXT Ctx, _In_reads_bytes_(Len) const UCHAR *Buf, _In_ SIZE_T Len);
_IRQL_requires_(PASSIVE_LEVEL)
VOID DcRxStart(_In_ PDEVICE_CONTEXT Ctx);

/* dchid.c: command/event layer and VHF devices */
_IRQL_requires_(PASSIVE_LEVEL)
VOID DchidHandlePacket(_In_ PDEVICE_CONTEXT Ctx, _In_ const dchid_hdr *Hdr, _In_ const UCHAR *Body);
EVT_WDF_WORKITEM DchidEvtJobWorker;
_IRQL_requires_(PASSIVE_LEVEL)
VOID DchidStart(_In_ PDEVICE_CONTEXT Ctx);
_IRQL_requires_(PASSIVE_LEVEL)
VOID DchidStop(_In_ PDEVICE_CONTEXT Ctx);
_IRQL_requires_(PASSIVE_LEVEL)
VOID DchidResetInterfaces(_In_ PDEVICE_CONTEXT Ctx);
