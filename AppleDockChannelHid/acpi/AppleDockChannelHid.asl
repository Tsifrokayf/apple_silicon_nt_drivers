// SPDX-License-Identifier: MIT
//
// AppleDockChannelHid.asl - example DSDT device node for AppleDockChannelHid
// on the MacBook Air 13" M2 (T8112 / J413). Fragment: place inside the
// \_SB scope of the platform DSDT.
//
// UNTESTED: syntax-checked with iasl only (see README.md); never booted.
//
// Values are from the Asahi Linux t8112 device tree (mtp_dockchannel and
// mtp_hid nodes, a_t8112.dtsi lines 1613-1646):
//   mtp_dockchannel IRQ block 0x2_4eb1_4000 + 0x4000, AIC IRQ 850 (level high)
//   mtp_hid "config" = 0x2_4eb3_0000 + 0x4000
//   mtp_hid "data"   = 0x2_4eb3_4000 + 0x4000
//
// The driver identifies the windows by ORDER: IRQ block, config, data.
//
// Not included on purpose:
//   - mtp_hid "rmt-config"/"rmt-data" (0x2_4eb2_8000 / 0x2_4eb2_c000): mapped
//     by the DT but never used by the reference driver.
//   - The MTP coprocessor itself (ASC 0x2_4e40_0000, SRAM 0x2_4ec0_0000,
//     mailbox 0x2_4e40_8000 with AIC 864-867, DART 0x2_4e80_8000 with
//     AIC 848). Those belong to a future RTKit helper device; once that
//     exists, this device should carry a _DEP on it so Windows starts it
//     second. Until then firmware must leave MTP running (driver README).
//
// Interrupt numbers: under the m1n1_windows hypervisor, Windows sees a
// virtual GICv3, so the value below must be the virtual GIC INTID. That
// INTID equals the AIC number when the AIC number is in 32..959; 850 is in
// that range, so it is used unchanged. (AIC lines >= 960 are remapped by
// the hypervisor and would need the remapped INTID instead.)
//
Device (DCHI)
{
    Name (_HID, "APPL0D01")     // project-defined ID, matched by the INF
    Name (_UID, Zero)

    Name (_CRS, ResourceTemplate ()
    {
        // 0: DockChannel IRQ block (IRQ_MASK +0x0, IRQ_FLAG +0x4)
        QWordMemory (ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                     NonCacheable, ReadWrite,
                     0x0, 0x24EB14000, 0x24EB17FFF, 0x0, 0x4000)
        // 1: HID FIFO config window (TX/RX thresholds)
        QWordMemory (ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                     NonCacheable, ReadWrite,
                     0x0, 0x24EB30000, 0x24EB33FFF, 0x0, 0x4000)
        // 2: HID FIFO data window (TX/RX data, TX_FREE, RX_COUNT)
        QWordMemory (ResourceConsumer, PosDecode, MinFixed, MaxFixed,
                     NonCacheable, ReadWrite,
                     0x0, 0x24EB34000, 0x24EB37FFF, 0x0, 0x4000)
        // DockChannel IRQ line: AIC 850 == virtual GIC INTID 850
        Interrupt (ResourceConsumer, Level, ActiveHigh, Exclusive) { 850 }
    })

    Method (_STA)
    {
        Return (0xF)
    }
}
