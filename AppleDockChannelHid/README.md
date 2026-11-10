# AppleDockChannelHid

A Windows 11 ARM64 KMDF driver for the internal keyboard and trackpad of the
MacBook Air 13" M2 (Apple T8112 SoC, board J413). It talks to the MTP
coprocessor's DockChannel FIFO, runs the DockChannel HID protocol, and creates
one Virtual HID Framework (VHF) device per HID interface the MTP reports. Each
VHF device uses the report descriptor the MTP sends.

## Status

**First version. It compiles and links. It has never been loaded on Windows
and never run on hardware.**

| What | State |
|---|---|
| `build/build.sh` builds `AppleDockChannelHid.sys` (ARM64, KMDF 1.33, /W4 /WX), optional test signing | done; build ran with 0 warnings and 0 errors, and `osslsigncode verify` passed |
| Protocol code (`src/dchid_proto.c`): framing, checksum, receive state machine, event parsers | unit-tested on Linux (`build/test.sh`, 142 checks, ASan/UBSan) against vectors built from the protocol description, not against captured traffic |
| Driver code (`driver.c`, `fifo.c`, `dchid.c`) | compiles only; no runtime test of any kind |
| `AppleDockChannelHid.inf` | never installed or InfVerif-checked; no catalog |
| `acpi/AppleDockChannelHid.asl` | compiles with iasl 2023 (inside a wrapper DefinitionBlock); never booted |

## What is implemented

- **ACPI binding and resources**: `ACPI\APPL0D01`. `EvtDevicePrepareHardware` maps
  three memory windows by order (DockChannel IRQ block, FIFO config, FIFO data)
  and creates the interrupt object from the single interrupt resource.
- **DockChannel FIFO transport** (`fifo.c`). Register layout and access rules
  follow the reference: 32-bit TX/RX words, byte tail via TX8/RX8 (RX8 data in
  bits 15:8), TX_FREE/RX_COUNT, TX/RX thresholds. The level-triggered line
  goes to an ISR. The ISR masks and clears the TX/RX sub-IRQ bits (2 and 3)
  and queues the WDF interrupt work item, which drains RX at PASSIVE_LEVEL or
  wakes a blocked sender.
- **Framing and checksum** (`dchid_proto.c`). This is the 8-byte header, the
  8-byte sub-header, the payload padded to 4 bytes, and a u32 checksum that
  makes the word sum 0xffffffff. A streaming receive state machine
  reassembles packets from arbitrary FIFO chunks. It drops bad headers and
  bad checksums the same way the reference does.
- **Command/event layer** (`dchid.c`). One command is outstanding at a time.
  A command is matched to its ACK by interface, sequence, flags and report
  id, with a 1 s timeout. Comm-interface events are handled as follows:
  - INIT: interface name, HID descriptor, GPIO request and product name are
    recorded.
  - Enable order: STM is enabled first. After STM is READY, its identity
    (feature report 0x10 → VID/PID/version) is read and the deferred
    interfaces are enabled.
  - READY creates the VHF device.
  - GPIO_CMD is always ACKed with `0xa1`. It ACKs with an error until SMC
    GPIO exists.
- **VHF**. There is one VHF device per interface with a descriptor. It is
  created when that interface reports READY, and input reports are forwarded
  unchanged with `VhfReadReportSubmit`. GetFeature, SetFeature and
  WriteReport requests are queued to the command worker and completed with
  `VhfAsyncOperationComplete`.
- **Teardown**:
  - D0Exit stops the worker, aborts any wait in progress, and completes
    every pending VHF operation.
  - The interrupt object is created in PrepareHardware, so WDF disconnects
    it and deletes it.
  - ReleaseHardware deletes all VHF devices with `VhfDelete(..., TRUE)`,
    frees the descriptors and unmaps the MMIO.
  - The work item and spin lock are parented to the device.

## What is stubbed (`src/platform.c`)

Each stub's comment states what the reference says must happen.

| Stub | Needs | Current behaviour |
|---|---|---|
| `DchidPlatformMtpEnsureRunning` / `MtpRelease` | an RTKit + ASC mailbox driver (MTP at 0x2_4e40_0000, mailbox 0x2_4e40_8000, AIC 864-867) and a DART driver for RTKit buffers | does nothing; **assumes firmware left MTP running** (unverified) |
| `DchidPlatformLoadFirmware` (multi-touch, `apple/tpmtfw-j413.bin`) | DMA through `mtp_dart` SID 1, plus a source for the firmware file (not found in the reference) | returns `STATUS_NOT_IMPLEMENTED`. The trackpad never becomes READY, so no trackpad VHF device appears |
| `DchidPlatformGpioPulse` (afe-reset = smc_gpio 8, stm-reset = smc_gpio 24, active low) | an SMC driver; the SMC key is **not found** (`DCHID_SMC_GPIO_KEY_UNKNOWN`) | returns `STATUS_NOT_IMPLEMENTED`; the device gets an error ACK (0xe000f00d) |
| `DchidPlatformSuspend` / `Resume` | knowledge of MTP behaviour across sleep (the reference has no PM) | logs only; D0Entry re-arms RX and hopes MTP kept its state |

So the realistic best case for this version is a working **keyboard**, and
only if MTP is already running when Windows starts.

## What the firmware / hypervisor must provide

1. A DSDT device with `_HID "APPL0D01"`. Its `_CRS` must list, in this order:
   - QWordMemory 0x2_4eb1_4000 + 0x4000 (DockChannel IRQ block)
   - QWordMemory 0x2_4eb3_0000 + 0x4000 (HID FIFO config)
   - QWordMemory 0x2_4eb3_4000 + 0x4000 (HID FIFO data)
   - one level-high `Interrupt` for AIC 850

   See `acpi/AppleDockChannelHid.asl`. Under the m1n1_windows hypervisor the
   interrupt number is the virtual GIC INTID. It equals the AIC number when
   that number is in 32..959, so 850 is used unchanged.
2. The MTP coprocessor booted or woken through RTKit before this driver starts
   (see the first stub above). The DT gives MTP no PMGR power domain.
3. Stage-2 mappings of those MMIO ranges that keep accesses non-posted (the DT
   marks the block `nonposted-mmio`), and delivery of AIC 850 as GIC INTID 850.

## Build

See `../build/README.md` for getting the WDK packages. Then:

```sh
WDK_ROOT=/path/to/wdkx build/build.sh                 # -> build/out/arm64/AppleDockChannelHid.sys
WDK_ROOT=... SIGN_CERT=test.crt SIGN_KEY=test.key build/build.sh   # also test-sign
build/test.sh                                         # host unit tests, needs only cc
```

## Installing (untested)

1. Boot Windows with test signing on and Secure Boot off:
   `bcdedit /set testsigning on`. Offline:
   `bcdedit /store <BCD> /set {default} testsigning on`.
2. Make sure the DSDT contains the device node, and copy
   `AppleDockChannelHid.sys` (test-signed) and `AppleDockChannelHid.inf` to
   the target.
3. Install with
   `pnputil /add-driver AppleDockChannelHid.inf /install`.
   No catalog is produced on Linux. With an embedded test signature, an
   unsigned-catalog install may need `inf2cat` and `signtool` on a Windows
   machine, or `DISM /Add-Driver /ForceUnsigned` for an offline image.
   Neither has been tried.
4. Debug output uses `DbgPrintEx(DPFLTR_IHVDRIVER_ID, ...)`. Enable it with
   `ed nt!Kd_IHVDRIVER_Mask 0xf` in a kernel debugger.

## Open questions

Each of these design decisions rests on something that could not be verified here:

- **Is MTP running at Windows start?** The RTKit stub assumes it. If m1n1 and
  the UEFI firmware do not leave it running, no INIT event ever arrives.
- **Non-posted MMIO.** It is not verified whether `MmMapIoSpaceEx(PAGE_NOCACHE)`
  gives Device-nGnRnE on Windows ARM64, or whether the hypervisor's stage 2
  enforces it. Posted writes to this block may fault on Apple silicon.
- **HID report type numbers** in the sub-header GROUP field (INPUT 0, OUTPUT 1,
  FEATURE 2). They come from Linux `enum hid_report_type`. That header was not
  among the reference files, so the values are from general knowledge.
- **IRQ flag semantics.** The code assumes that a sub-IRQ whose condition still
  holds re-asserts after unmask (level behaviour), and that the thresholds
  mean "at least N bytes / N free". The Linux driver relies on the same
  thing, but it is not documented.
- **VHF usage details**, all unverified:
  - several `VhfCreate` calls on one device object;
  - `VhfCreate` from a work item after start;
  - explicit `InstanceID` per interface;
  - `VhfReadReportSubmit` copying the buffer when no
    `EvtVhfReadyForNextReadReport` is given;
  - `VhfDelete(TRUE)` inside `EvtDeviceReleaseHardware`;
  - the inbox `vhf` service being present for `LowerFilters`.
- **VHF device created on READY.** This is forced by VHF having no "open"
  callback, which is where Linux waits for READY. It assumes every usable
  interface sends READY after being enabled.
- **SET_REPORT type.** Linux sends the OUTPUT type for every SET, feature
  sets included. This driver does the same, which may be a Linux quirk.
- **GPIO at start.** Linux refuses to open an interface whose requested GPIO is
  missing. This driver only fails the later GPIO command, because it has no
  GPIO at all. Whether STM or the keyboard misbehave after an error ACK is
  unknown.
- **PnP stop/restart.** Interface state is dropped in ReleaseHardware. The MTP
  is not known to resend INIT, so after a rebalance the devices would not
  return. A way to re-trigger INIT (an MTP restart through RTKit?) is unknown.
- **WDF flushes the interrupt work item on disconnect.** This is assumed, so
  MMIO is unmapped only after the RX path stops.
- **Usefulness of the descriptors.** Apple's multitouch descriptor is not a
  Windows Precision Touchpad collection, so even with firmware the trackpad
  would need translation. Whether `kbdhid` binds to the keyboard descriptor
  as-is is unknown.

## Licence

This driver is new code under the MIT licence (the repository default).

The protocol and register knowledge comes from the Asahi Linux sources
`drivers/soc/apple/dockchannel.c` (GPL-2.0-only OR MIT),
`drivers/hid/dockchannel-hid/dockchannel-hid.c` (GPL-2.0 OR MIT),
`drivers/soc/apple/rtkit-helper.c` (GPL-2.0-only OR MIT) and the t8112/J413
device trees. They were used as a protocol reference only. No code was
copied: structures are re-expressed as explicit little-endian decoding and
the control flow is Windows-specific. Comments cite the reference by file
and line. Because the references are dual-licensed with MIT, MIT is
compatible even if the driver were judged derivative. Credit: The Asahi
Linux Contributors. The repository owner should confirm this choice.
