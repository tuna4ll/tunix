DefinitionBlock ("", "DSDT", 2, "TUNIX", "MACHINE", 1)
{
    Name (PPCV, Zero)
    Name (PDCC, Zero)
    Name (ECON, Zero)
    Name (DOSA, 0xFF)

    Scope (\_SB)
    {
        Device (PCI0)
        {
            Name (_HID, EisaId ("PNP0A08"))
            Name (_CID, EisaId ("PNP0A03"))

            Device (LPCB)
            {
                Name (_ADR, 0x001F0000)

                Device (EC0)
                {
                    Name (_HID, EisaId ("PNP0C09"))
                    Name (_UID, One)
                    Name (_GPE, 0x16)
                    Name (_CRS, ResourceTemplate ()
                    {
                        IO (Decode16, 0x0062, 0x0062, 0x00, 0x01)
                        IO (Decode16, 0x0066, 0x0066, 0x00, 0x01)
                    })

                    OperationRegion (ERAM, EmbeddedControl, Zero, 0xFF)
                    Field (ERAM, ByteAcc, NoLock, Preserve)
                    {
                        Offset (0x4E),
                        LIDS,   1,
                        Offset (0x58),
                        RTMP,   8,
                        Offset (0x90),
                        THRL,   8,
                        Offset (0xA3),
                        CBSC,   8
                    }

                    Method (_REG, 2, NotSerialized)
                    {
                        If ((Arg0 == 0x03))
                        {
                            ECON = Arg1
                        }
                    }

                    Method (_Q60, 0, NotSerialized)
                    {
                        Notify (\_SB.WMI2, 0x80)
                        If ((CBSC == 0x04))
                        {
                            Notify (^^^GFX0.LCD0, 0x87)
                            Notify (^^^PEG0.VGA.LCD0, 0x87)
                        }

                        If ((CBSC == 0x05))
                        {
                            Notify (^^^GFX0.LCD0, 0x86)
                            Notify (^^^PEG0.VGA.LCD0, 0x86)
                        }
                    }

                    Method (_Q8E, 0, NotSerialized)
                    {
                        PPCV = THRL
                        Notify (\_PR.CPU0, 0x80)
                    }

                    Method (_Q8A, 0, NotSerialized)
                    {
                        Notify (\_SB.LID0, 0x80)
                    }

                    Method (_Q80, 0, NotSerialized)
                    {
                        Notify (\_TZ.TZ01, 0x80)
                    }
                }
            }

            Device (GFX0)
            {
                Name (_ADR, 0x00020000)
                Method (_DOS, 1, NotSerialized)
                {
                    DOSA = Arg0
                }

                Device (LCD0)
                {
                    Name (_ADR, 0x0400)
                    Method (_BCM, 1, NotSerialized)
                    {
                    }
                }
            }

            Device (PEG0)
            {
                Name (_ADR, 0x00010000)
                Device (VGA)
                {
                    Name (_ADR, Zero)
                    Method (_DOS, 1, NotSerialized)
                    {
                        DOSA = Arg0
                    }

                    Device (LCD0)
                    {
                        Name (_ADR, 0x0110)
                        Method (_BCM, 1, NotSerialized)
                        {
                        }
                    }
                }
            }
        }

        Device (WMI2)
        {
            Name (_HID, EisaId ("PNP0C14"))
            Name (_UID, 0x02)
        }

        Device (LID0)
        {
            Name (_HID, EisaId ("PNP0C0D"))
            Method (_LID, 0, NotSerialized)
            {
                Return (\_SB.PCI0.LPCB.EC0.LIDS)
            }
        }
    }

    Scope (\_PR)
    {
        Processor (CPU0, 0x01, 0x00000410, 0x06)
        {
            Method (_PDC, 1, NotSerialized)
            {
                CreateDWordField (Arg0, 0x08, CAPS)
                PDCC = CAPS
            }

            Name (_PSS, Package ()
            {
                Package () { 2267, 35000, 10, 10, 0x11, 0x11 },
                Package () { 1600, 25000, 10, 10, 0x0C, 0x0C },
                Package () { 1200, 15000, 10, 10, 0x09, 0x09 }
            })

            Method (_PPC, 0, NotSerialized)
            {
                Return (PPCV)
            }
        }
    }

    Scope (\_TZ)
    {
        ThermalZone (TZ01)
        {
            Method (_TMP, 0, NotSerialized)
            {
                Return (((\_SB.PCI0.LPCB.EC0.RTMP * 0x0A) + 0x0AAC))
            }

            Method (_PSV, 0, NotSerialized)
            {
                Return (0x0E62)
            }

            Method (_CRT, 0, NotSerialized)
            {
                Return (0x0EC6)
            }
        }
    }
}
