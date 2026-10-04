// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

namespace LibOSDP;

/// <summary>An event a PD reported to a <see cref="ControlPanel"/>.</summary>
/// <param name="address">Address of the PD that sent it.</param>
/// <param name="ev">The event.</param>
public sealed class OsdpEventArgs(int address, OsdpEvent ev) : EventArgs
{
    /// <summary>Address of the PD that sent it.</summary>
    public int Address { get; } = address;

    /// <summary>The event.</summary>
    public OsdpEvent Event { get; } = ev;
}
