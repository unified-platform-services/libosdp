// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

using LibOSDP.Interop;

namespace LibOSDP;

/// <summary>The identity a PD reports (OSDP osdp_PDID).</summary>
/// <param name="Version">Version of the PD's protocol implementation.</param>
/// <param name="Model">Manufacturer's model number.</param>
/// <param name="VendorCode">IEEE OUI of the vendor (low 24 bits).</param>
/// <param name="SerialNumber">Serial number.</param>
/// <param name="FirmwareVersion">Firmware version: major, minor, build in the low three bytes.</param>
public sealed record PdId(int Version = 1, int Model = 0, uint VendorCode = 0,
                          uint SerialNumber = 0, uint FirmwareVersion = 0)
{
    internal NativePdId ToNative() => new()
    {
        Version = Version,
        Model = Model,
        VendorCode = VendorCode,
        SerialNumber = SerialNumber,
        FirmwareVersion = FirmwareVersion,
    };

    internal static PdId FromNative(in NativePdId id) =>
        new(id.Version, id.Model, id.VendorCode, id.SerialNumber, id.FirmwareVersion);
}
