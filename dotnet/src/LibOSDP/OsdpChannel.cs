// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

namespace LibOSDP;

/// <summary>
/// A byte transport between a control panel and its peripheral devices, such
/// as an RS-485 serial port. LibOSDP calls these methods from its refresh
/// loop, so none of them may block.
/// </summary>
public abstract class OsdpChannel : IDisposable
{
    /// <summary>Copy received bytes into <paramref name="buffer"/> without waiting.</summary>
    /// <param name="buffer">Where to put them.</param>
    /// <returns>Bytes copied; 0 when nothing has arrived; negative on a transport error.</returns>
    public abstract int Read(Span<byte> buffer);

    /// <summary>Send <paramref name="buffer"/>.</summary>
    /// <param name="buffer">Bytes to send.</param>
    /// <returns>Bytes sent; negative on a transport error.</returns>
    public abstract int Write(ReadOnlySpan<byte> buffer);

    /// <summary>Drop any received bytes not yet read. Must not block.</summary>
    public virtual void Flush() { }

    /// <summary>Release the transport.</summary>
    public void Dispose()
    {
        Dispose(true);
        GC.SuppressFinalize(this);
    }

    /// <summary>Release the transport.</summary>
    /// <param name="disposing">True when called from <see cref="Dispose()"/>.</param>
    protected virtual void Dispose(bool disposing) { }
}
