// SPDX-License-Identifier: MIT
/*
 * platform.c - isolated dependencies on drivers and firmware that do not
 * exist yet for Windows on Apple silicon. Nothing in this file has been
 * run. Each function says what the reference requires and why it is a stub.
 */
#include "platform.h"

/*
 * MTP coprocessor start/wake.
 *
 * Reference [RH:76-120, RT:9-75, T:1576-1601]: MTP is an RTKit "helper"
 * ASC at 0x24e400000 (SRAM 0x24ec00000, 1 MiB) with an asc-mailbox-v4 at
 * 0x24e408000 (AIC 864-867). Linux, before the HID driver may touch the
 * FIFO [H:1144-1170]:
 *   1. initialises RTKit on that mailbox (HELLO / EPMAP / STARTEP for the
 *      system endpoints; versions 11-12);
 *   2. writes ASC CPU_CONTROL (asc + 0x44) = RUN (bit 4);
 *   3. calls apple_rtkit_wake ("works for both wake and boot");
 *   4. serves RTKit buffer requests: iova 0 -> allocate DMA memory mapped
 *      through mtp_dart SID 1; nonzero iova -> must lie inside MTP SRAM.
 * The OS loads no MTP firmware image; it is expected to be resident
 * already (who loads it is not stated in the reference).
 *
 * Why a stub: steps 1-4 belong in a separate RTKit/mailbox driver, and step 4
 * needs a DART driver that Windows does not have. Writing CPU_CONTROL here
 * without the RTKit handshake would not be a correct partial implementation.
 *
 * Current behaviour: does nothing and returns success, i.e. this driver
 * ASSUMES an earlier boot stage (m1n1 or UEFI) left MTP running. Whether
 * m1n1_windows or the Mu firmware does that has NOT been verified. If it
 * does not, no INIT event ever arrives and no HID device appears.
 */
NTSTATUS DchidPlatformMtpEnsureRunning(PDEVICE_CONTEXT Ctx)
{
    UNREFERENCED_PARAMETER(Ctx);
    DCHID_WARN("%s", "TODO: MTP RTKit wake not implemented; assuming firmware left MTP running");
    return STATUS_SUCCESS;
}

/*
 * Reference [RH:123-131]: on removal Linux quiesces RTKit if running and
 * writes ASC CPU_CONTROL = 0. Not done here for the reason given above; MTP
 * is left in whatever state it was in.
 */
VOID DchidPlatformMtpRelease(PDEVICE_CONTEXT Ctx)
{
    UNREFERENCED_PARAMETER(Ctx);
    DCHID_INFO("%s", "TODO: MTP RTKit quiesce not implemented; MTP left running");
}

/*
 * Which interfaces need firmware. Linux keys this on a "firmware-name"
 * property in the interface's DT child node [H:415-421]; J413 sets it only
 * for "multi-touch" = "apple/tpmtfw-j413.bin" [J:240-242]. There is no DT
 * here, so the J413 table is compiled in.
 */
BOOLEAN DchidPlatformIfaceNeedsFirmware(const DCHID_IFACE *Iface)
{
    return strcmp(Iface->Name, "multi-touch") == 0;
}

/*
 * Multi-touch firmware upload.
 *
 * Reference [H:378-455, H:480-529, H:125-134]:
 *   1. load "apple/tpmtfw-j413.bin"; header {u32 magic 0x46444948 "HIDF",
 *      u32 version 1, u32 hdr_length, u32 data_length, u32 iface_offset};
 *      payload = data_length bytes after hdr_length; if iface_offset != 0,
 *      patch payload[iface_offset] = interface index;
 *   2. copy the payload into a DMA buffer the MTP can reach, i.e. mapped
 *      through mtp_dart SID 1 (64-bit DMA mask) [H:394-401, H:1102, T:1638];
 *   3. comm command {0x95 SEND_FIRMWARE, 2, 0, iface, u64 dma_addr,
 *      u32 size} (feature SET_REPORT);
 *   4. comm commands RESET_INTERFACE {0x40, 1, iface, 0} then
 *      {0x40, 1, iface, 2};
 *   5. wait for READY of that interface (<= 2000 ms) [H:24, H:554-572].
 *
 * Why a stub: (a) where the firmware file comes from is not found in the
 * reference (it originates from macOS; no extraction procedure here);
 * (b) Windows has no driver for mtp_dart, so there is no way to obtain a
 * device address the MTP can use. Unless firmware left the DART in bypass
 * (not verified), passing a CPU physical address would be wrong.
 *
 * Result: fails cleanly; the multi-touch interface is enabled but never
 * becomes READY, so no VHF device is created for it.
 */
NTSTATUS DchidPlatformLoadFirmware(PDEVICE_CONTEXT Ctx, PDCHID_IFACE Iface)
{
    UNREFERENCED_PARAMETER(Ctx);
    DCHID_ERR("TODO: firmware upload for '%s' not implemented (needs MTP DART + firmware file)",
              Iface->Name);
    return STATUS_NOT_IMPLEMENTED;
}

/*
 * SMC GPIO reset lines.
 *
 * Reference: the device asks for a GPIO by name in INIT (e.g. "stm-reset")
 * and later sends GPIO_CMD; Linux resolves the name to the DT property
 * "apple,<name>-gpios" [H:457-478]. On J413 [J:236-238]:
 *   apple,afe-reset-gpios = smc_gpio 8, active low
 *   apple,stm-reset-gpios = smc_gpio 24, active low
 * Command 3 = pulse: assert, ~10 ms ("random guess" in the reference),
 * deassert [H:891-898]. smc_gpio is an SMC child (SMC RTKit endpoint 0x20)
 * [T:1426-1430]; the SMC key that drives a given GPIO is NOT FOUND in the
 * reference (m1n1 writes u32 0x800001 to a key named by an ADT "function-*"
 * property for other GPIOs, which is not enough to derive these).
 */
/* TODO: SMC key for smc_gpio lines - not found in the reference. 0 = unknown. */
#define DCHID_SMC_GPIO_KEY_UNKNOWN 0u

typedef struct _DCHID_SMC_GPIO {
    const char *Name;
    ULONG       SmcGpio;
    BOOLEAN     ActiveLow;
    ULONG       SmcKey;
} DCHID_SMC_GPIO;

static const DCHID_SMC_GPIO DchidJ413Gpios[] = {
    { "afe-reset", 8,  TRUE, DCHID_SMC_GPIO_KEY_UNKNOWN },
    { "stm-reset", 24, TRUE, DCHID_SMC_GPIO_KEY_UNKNOWN },
};

NTSTATUS DchidPlatformGpioPulse(PDEVICE_CONTEXT Ctx, const DCHID_IFACE *Iface)
{
    ULONG i;

    UNREFERENCED_PARAMETER(Ctx);

    for (i = 0; i < ARRAYSIZE(DchidJ413Gpios); i++) {
        const DCHID_SMC_GPIO *g = &DchidJ413Gpios[i];

        if (strcmp(g->Name, Iface->GpioName) != 0)
            continue;
        if (g->SmcKey == DCHID_SMC_GPIO_KEY_UNKNOWN)
            DCHID_ERR("TODO: SMC key for smc_gpio %lu (%s, active %s) unknown",
                      g->SmcGpio, g->Name, g->ActiveLow ? "low" : "high");
        else
            DCHID_ERR("TODO: no SMC driver to pulse smc_gpio %lu (%s)", g->SmcGpio, g->Name);
        return STATUS_NOT_IMPLEMENTED;
    }
    DCHID_ERR("GPIO '%s' requested by '%s' is not in the J413 table",
              Iface->GpioName, Iface->Name);
    return STATUS_NOT_FOUND;
}

/*
 * Power. The reference HID driver has no suspend/resume at all (its remove
 * is BUG_ON(1) [H:1193-1196]) and the DT gives MTP no power domain
 * [T:1576-1586], so there is nothing to copy. What device state survives a
 * system sleep, and whether the MTP must be re-woken through RTKit, is
 * unknown. These hooks only log; D0Entry re-arms the receive path and hopes
 * MTP kept its state (unverified).
 */
VOID DchidPlatformSuspend(PDEVICE_CONTEXT Ctx, WDF_POWER_DEVICE_STATE TargetState)
{
    UNREFERENCED_PARAMETER(Ctx);
    DCHID_WARN("TODO: leaving D0 for state %d; MTP power handling not implemented",
               (int)TargetState);
}

VOID DchidPlatformResume(PDEVICE_CONTEXT Ctx, WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(Ctx);
    DCHID_WARN("TODO: returning to D0 from state %d; MTP state after sleep unknown",
               (int)PreviousState);
}
