// SPDX-License-Identifier: MIT
/*
 * platform.h - everything this driver needs from other (not yet existing)
 * drivers or from firmware. Every function here is a stub; see platform.c
 * for what the reference says each one must eventually do.
 */
#pragma once

#include "driver.h"

/* RTKit wake of the MTP coprocessor (needs an RTKit/ASC mailbox driver). */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS DchidPlatformMtpEnsureRunning(_In_ PDEVICE_CONTEXT Ctx);
_IRQL_requires_(PASSIVE_LEVEL)
VOID DchidPlatformMtpRelease(_In_ PDEVICE_CONTEXT Ctx);

/* Multi-touch firmware upload (needs file access policy and MTP DART DMA). */
BOOLEAN DchidPlatformIfaceNeedsFirmware(_In_ const DCHID_IFACE *Iface);
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS DchidPlatformLoadFirmware(_In_ PDEVICE_CONTEXT Ctx, _In_ PDCHID_IFACE Iface);

/* SMC GPIO reset lines (needs an SMC driver). */
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS DchidPlatformGpioPulse(_In_ PDEVICE_CONTEXT Ctx, _In_ const DCHID_IFACE *Iface);

/* Power transitions other than first start / final removal. */
_IRQL_requires_(PASSIVE_LEVEL)
VOID DchidPlatformSuspend(_In_ PDEVICE_CONTEXT Ctx, _In_ WDF_POWER_DEVICE_STATE TargetState);
_IRQL_requires_(PASSIVE_LEVEL)
VOID DchidPlatformResume(_In_ PDEVICE_CONTEXT Ctx, _In_ WDF_POWER_DEVICE_STATE PreviousState);
