// SPDX-License-Identifier: MIT
/*
 * driver.c - DriverEntry, device creation, ACPI resource parsing and the
 * PnP/power callbacks of AppleDockChannelHid.
 *
 * Expected ACPI resources, in this order (see ../acpi/AppleDockChannelHid.asl):
 *   memory 0: DockChannel IRQ block   (J413: 0x2_4eb1_4000, 0x4000)
 *   memory 1: HID FIFO config window  (J413: 0x2_4eb3_0000, 0x4000)
 *   memory 2: HID FIFO data window    (J413: 0x2_4eb3_4000, 0x4000)
 *   interrupt: DockChannel IRQ line, level high (J413: AIC 850)
 * [T:1613-1646]. Order is the only way to tell the windows apart: the
 * resources carry no names, unlike the DT's reg-names.
 *
 * Untested: never loaded on Windows, never run on hardware.
 */
#include "platform.h"

static EVT_WDF_DRIVER_DEVICE_ADD DchidEvtDeviceAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE DchidEvtPrepareHardware;
static EVT_WDF_DEVICE_RELEASE_HARDWARE DchidEvtReleaseHardware;
static EVT_WDF_DEVICE_D0_ENTRY_POST_INTERRUPTS_ENABLED DchidEvtD0EntryPostInterruptsEnabled;
static EVT_WDF_DEVICE_D0_EXIT_PRE_INTERRUPTS_DISABLED DchidEvtD0ExitPreInterruptsDisabled;

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;

    WDF_DRIVER_CONFIG_INIT(&config, DchidEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config,
                           WDF_NO_HANDLE);
}

static NTSTATUS DchidEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_WORKITEM_CONFIG wiConfig;
    WDFDEVICE device;
    PDEVICE_CONTEXT ctx;
    NTSTATUS st;

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = DchidEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = DchidEvtReleaseHardware;
    pnp.EvtDeviceD0EntryPostInterruptsEnabled = DchidEvtD0EntryPostInterruptsEnabled;
    pnp.EvtDeviceD0ExitPreInterruptsDisabled = DchidEvtD0ExitPreInterruptsDisabled;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, DEVICE_CONTEXT);
    st = WdfDeviceCreate(&DeviceInit, &attr, &device);
    if (!NT_SUCCESS(st))
        return st;

    /* Child objects below are parented to the device and die with it. */
    ctx = DchidGetContext(device);
    ctx->Device = device;
    ctx->Stopping = TRUE;  /* no jobs until the first D0 entry */
    InitializeListHead(&ctx->Jobs);
    KeInitializeEvent(&ctx->TxEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&ctx->AckEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&ctx->StopEvent, NotificationEvent, FALSE);

    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = device;
    st = WdfSpinLockCreate(&attr, &ctx->Lock);
    if (!NT_SUCCESS(st))
        return st;

    WDF_WORKITEM_CONFIG_INIT(&wiConfig, DchidEvtJobWorker);
    wiConfig.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    attr.ParentObject = device;
    st = WdfWorkItemCreate(&wiConfig, &attr, &ctx->JobWorker);
    if (!NT_SUCCESS(st))
        return st;

    DchidResetInterfaces(ctx);
    return STATUS_SUCCESS;
}

static NTSTATUS DchidMapMemory(PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc, PUCHAR *Base, SIZE_T *Len)
{
    PHYSICAL_ADDRESS pa;
    ULONGLONG len = RtlCmDecodeMemIoResource(Desc, (PULONGLONG)&pa.QuadPart);

    if (len == 0 || len > MAXULONG)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    /*
     * Plain uncached device mapping. The DT marks this block
     * "nonposted-mmio" [T:1621]; whether PAGE_NOCACHE yields a non-posted
     * (nGnRnE) mapping on Windows ARM64, or the hypervisor's stage 2 makes
     * it so, is not verified (README, "Open questions").
     */
    *Base = MmMapIoSpaceEx(pa, (SIZE_T)len, PAGE_READWRITE | PAGE_NOCACHE);
    if (*Base == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    *Len = (SIZE_T)len;
    return STATUS_SUCCESS;
}

static VOID DchidUnmapAll(PDEVICE_CONTEXT Ctx)
{
    if (Ctx->IrqBase)
        MmUnmapIoSpace(Ctx->IrqBase, Ctx->IrqLen);
    if (Ctx->CfgBase)
        MmUnmapIoSpace(Ctx->CfgBase, Ctx->CfgLen);
    if (Ctx->DataBase)
        MmUnmapIoSpace(Ctx->DataBase, Ctx->DataLen);
    Ctx->IrqBase = Ctx->CfgBase = Ctx->DataBase = NULL;
    Ctx->IrqLen = Ctx->CfgLen = Ctx->DataLen = 0;
    if (Ctx->RxBody) {
        ExFreePoolWithTag(Ctx->RxBody, DCHID_POOL_TAG);
        Ctx->RxBody = NULL;
    }
}

static NTSTATUS DchidParseResources(PDEVICE_CONTEXT Ctx, WDFCMRESLIST Raw, WDFCMRESLIST Trans)
{
    PUCHAR *bases[3] = { &Ctx->IrqBase, &Ctx->CfgBase, &Ctx->DataBase };
    SIZE_T *lens[3] = { &Ctx->IrqLen, &Ctx->CfgLen, &Ctx->DataLen };
    ULONG nmem = 0, count = WdfCmResourceListGetCount(Trans), i;
    WDF_INTERRUPT_CONFIG ic;
    NTSTATUS st;

    for (i = 0; i < count; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = WdfCmResourceListGetDescriptor(Trans, i);

        if (d == NULL)
            continue;
        switch (d->Type) {
        case CmResourceTypeMemory:
        case CmResourceTypeMemoryLarge:
            if (nmem >= ARRAYSIZE(bases)) {
                DCHID_WARN("extra memory resource %lu ignored", i);
                break;
            }
            st = DchidMapMemory(d, bases[nmem], lens[nmem]);
            if (!NT_SUCCESS(st))
                return st;
            nmem++;
            break;
        case CmResourceTypeInterrupt:
            if (Ctx->Interrupt != NULL) {
                DCHID_WARN("extra interrupt resource %lu ignored", i);
                break;
            }
            /* Created here so it is bound to this exact resource (KMDF >= 1.11). */
            WDF_INTERRUPT_CONFIG_INIT(&ic, DcEvtInterruptIsr, NULL);
            ic.EvtInterruptWorkItem = DcEvtInterruptWorkItem;
            ic.EvtInterruptEnable = DcEvtInterruptEnable;
            ic.EvtInterruptDisable = DcEvtInterruptDisable;
            ic.InterruptRaw = WdfCmResourceListGetDescriptor(Raw, i);
            ic.InterruptTranslated = d;
            st = WdfInterruptCreate(Ctx->Device, &ic, WDF_NO_OBJECT_ATTRIBUTES, &Ctx->Interrupt);
            if (!NT_SUCCESS(st))
                return st;
            break;
        default:
            break;
        }
    }

    if (nmem < ARRAYSIZE(bases) || Ctx->Interrupt == NULL) {
        DCHID_ERR("need 3 memory resources and 1 interrupt, got %lu and %u",
                  nmem, Ctx->Interrupt ? 1u : 0u);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (Ctx->IrqLen < DC_IRQ_FLAG + 4 || Ctx->CfgLen < DC_CFG_RX_THRESH + 4 ||
        Ctx->DataLen < DC_DATA_MIN_LEN) {
        DCHID_ERR("%s", "memory resource too small");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS DchidEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST ResourcesRaw,
                                        WDFCMRESLIST ResourcesTranslated)
{
    PDEVICE_CONTEXT ctx = DchidGetContext(Device);
    NTSTATUS st;

    st = DchidParseResources(ctx, ResourcesRaw, ResourcesTranslated);
    if (!NT_SUCCESS(st))
        goto fail;

    ctx->RxBody = ExAllocatePool2(POOL_FLAG_NON_PAGED, DCHID_MAX_BODY, DCHID_POOL_TAG);
    if (ctx->RxBody == NULL) {
        st = STATUS_INSUFFICIENT_RESOURCES;
        goto fail;
    }
    if (dchid_rx_init(&ctx->Rx, ctx->RxBody, DCHID_MAX_BODY) != 0) {
        st = STATUS_INTERNAL_ERROR;
        goto fail;
    }

    st = DchidPlatformMtpEnsureRunning(ctx);
    if (!NT_SUCCESS(st))
        goto fail;
    ctx->MtpRunning = TRUE;
    return STATUS_SUCCESS;

fail:
    /*
     * Undo our own mappings here rather than relying on ReleaseHardware
     * being called after a failed PrepareHardware. The interrupt object,
     * if created, is owned and deleted by the framework.
     */
    DCHID_ERR("PrepareHardware failed 0x%08x", st);
    DchidUnmapAll(ctx);
    ctx->Interrupt = NULL;
    return st;
}

static NTSTATUS DchidEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST ResourcesTranslated)
{
    PDEVICE_CONTEXT ctx = DchidGetContext(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    /* D0Exit has normally done this already; DchidStop is idempotent. */
    DchidStop(ctx);
    DchidResetInterfaces(ctx);
    if (ctx->MtpRunning) {
        DchidPlatformMtpRelease(ctx);
        ctx->MtpRunning = FALSE;
    }
    DchidUnmapAll(ctx);
    /* The framework deletes interrupt objects created in PrepareHardware. */
    ctx->Interrupt = NULL;
    return STATUS_SUCCESS;
}

static NTSTATUS DchidEvtD0EntryPostInterruptsEnabled(WDFDEVICE Device,
                                                     WDF_POWER_DEVICE_STATE PreviousState)
{
    PDEVICE_CONTEXT ctx = DchidGetContext(Device);

    if (PreviousState != WdfPowerDeviceD3Final)
        DchidPlatformResume(ctx, PreviousState);
    DchidStart(ctx);
    DcRxStart(ctx);
    return STATUS_SUCCESS;
}

static NTSTATUS DchidEvtD0ExitPreInterruptsDisabled(WDFDEVICE Device,
                                                    WDF_POWER_DEVICE_STATE TargetState)
{
    PDEVICE_CONTEXT ctx = DchidGetContext(Device);

    /* Stop commands and pending waits before the interrupt goes away. */
    DchidStop(ctx);
    if (TargetState != WdfPowerDeviceD3Final)
        DchidPlatformSuspend(ctx, TargetState);
    return STATUS_SUCCESS;
}
