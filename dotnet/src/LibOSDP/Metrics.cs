// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

using LibOSDP.Interop;

namespace LibOSDP;

/// <summary>Link counters since the previous snapshot; taking one resets them.</summary>
/// <param name="PacketsSent">Packets sent.</param>
/// <param name="PacketsReceived">Well-formed frames received, including those that failed the CRC or checksum.</param>
/// <param name="PacketCheckErrors">Frames rejected for a bad checksum or CRC.</param>
/// <param name="NakCount">NAKs sent (PD) or received (CP).</param>
/// <param name="ScHandshakeCount">Successful secure channel activations.</param>
/// <param name="ScFailureCount">Tear-downs of a previously active secure channel session.</param>
/// <param name="CommandCount">Commands handled.</param>
/// <param name="EventCount">Events handled.</param>
public sealed record Metrics(uint PacketsSent, uint PacketsReceived, uint PacketCheckErrors, uint NakCount,
                             uint ScHandshakeCount, uint ScFailureCount, uint CommandCount, uint EventCount)
{
    internal static Metrics FromNative(in NativeMetrics m) =>
        new(m.PacketsSent, m.PacketsReceived, m.PacketCheckErrors, m.NakCount,
            m.ScHandshakeCount, m.ScFailureCount, m.CommandCount, m.EventCount);
}

/// <summary>Progress of a running file transfer.</summary>
/// <param name="Size">File size in bytes.</param>
/// <param name="Offset">Bytes transferred so far.</param>
public sealed record FileTransferStatus(uint Size, uint Offset);
