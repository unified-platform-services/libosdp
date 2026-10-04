// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

namespace LibOSDP;

/// <summary>
/// One end of an in-memory link: what one end writes, the other reads. Use a
/// pair to run a control panel and a peripheral device in one process, for
/// tests or simulation.
/// </summary>
public sealed class LoopbackChannel : OsdpChannel
{
    private readonly Queue<byte> _inbox = new();
    private LoopbackChannel _peer = null!;

    private LoopbackChannel() { }

    /// <summary>Create two connected ends.</summary>
    /// <returns>The two ends; give one to each device.</returns>
    public static (LoopbackChannel First, LoopbackChannel Second) CreatePair()
    {
        var first = new LoopbackChannel();
        var second = new LoopbackChannel();
        first._peer = second;
        second._peer = first;
        return (first, second);
    }

    /// <inheritdoc/>
    public override int Read(Span<byte> buffer)
    {
        lock (_inbox)
        {
            int n = Math.Min(buffer.Length, _inbox.Count);
            for (int i = 0; i < n; i++)
                buffer[i] = _inbox.Dequeue();
            return n;
        }
    }

    /// <inheritdoc/>
    public override int Write(ReadOnlySpan<byte> buffer)
    {
        Queue<byte> inbox = _peer._inbox;
        lock (inbox)
        {
            foreach (byte b in buffer)
                inbox.Enqueue(b);
        }
        return buffer.Length;
    }

    /// <inheritdoc/>
    public override void Flush()
    {
        lock (_inbox)
            _inbox.Clear();
    }
}
