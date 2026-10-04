// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

using LibOSDP.Interop;

namespace LibOSDP;

/// <summary>
/// Describes one peripheral device: the PDs a control panel manages, or the
/// PD this process is.
/// </summary>
/// <param name="Address">Bus address, 0-126.</param>
public sealed record PdInfo(int Address)
{
    /// <summary>Label used in logs; defaults to <c>PD-</c> followed by the address.</summary>
    public string? Name { get; init; }

    /// <summary>Bus baud rate.</summary>
    public uint BaudRate { get; init; } = 9600;

    /// <summary>Behaviour flags.</summary>
    public OsdpFlags Flags { get; init; }

    /// <summary>
    /// The 16-byte secure channel base key, or null to run in plain text
    /// (unless <see cref="OsdpFlags.InstallMode"/> is set on a PD).
    /// </summary>
    public byte[]? Scbk { get; init; }

    /// <summary>The identity this PD reports. Ignored by a control panel.</summary>
    public PdId Id { get; init; } = new();

    internal void Validate()
    {
        ArgumentOutOfRangeException.ThrowIfNegative(Address, nameof(Address));
        ArgumentOutOfRangeException.ThrowIfGreaterThan(Address, 126, nameof(Address));
        if (Scbk is not null && Scbk.Length != 16)
            throw new ArgumentException("Scbk must be exactly 16 bytes.", nameof(Scbk));
    }
}

/// <summary>One capability a PD advertises (OSDP osdp_PDCAP entry).</summary>
/// <param name="FunctionCode">Which capability.</param>
/// <param name="ComplianceLevel">How it is supported; meaning depends on the function code.</param>
/// <param name="NumItems">How many, e.g. number of outputs.</param>
public readonly record struct PdCapability(Capability FunctionCode, byte ComplianceLevel, byte NumItems);
