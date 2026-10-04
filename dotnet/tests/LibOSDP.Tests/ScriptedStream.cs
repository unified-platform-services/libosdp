// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

using System.Threading.Channels;

namespace LibOSDP.Tests;

/// A duplex stream whose reads block until the test feeds data, like a serial port.
internal sealed class ScriptedStream : Stream
{
    private readonly Channel<byte[]> _incoming = Channel.CreateUnbounded<byte[]>();
    private readonly MemoryStream _written = new();

    public bool Disposed { get; private set; }

    public byte[] Written
    {
        get { lock (_written) return _written.ToArray(); }
    }

    public void Feed(params byte[] data) => _incoming.Writer.TryWrite(data);

    public void End() => _incoming.Writer.TryComplete();

    public override async ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default)
    {
        if (!await _incoming.Reader.WaitToReadAsync(cancellationToken))
            return 0;
        byte[] chunk = await _incoming.Reader.ReadAsync(cancellationToken);
        chunk.CopyTo(buffer);
        return chunk.Length;
    }

    public override int Read(byte[] buffer, int offset, int count) =>
        ReadAsync(buffer.AsMemory(offset, count)).AsTask().GetAwaiter().GetResult();

    public override void Write(byte[] buffer, int offset, int count) => Write(buffer.AsSpan(offset, count));

    public override void Write(ReadOnlySpan<byte> buffer)
    {
        lock (_written) _written.Write(buffer);
    }

    protected override void Dispose(bool disposing)
    {
        Disposed = true;
        End();
        base.Dispose(disposing);
    }

    public override bool CanRead => true;
    public override bool CanWrite => true;
    public override bool CanSeek => false;
    public override long Length => throw new NotSupportedException();
    public override long Position { get => throw new NotSupportedException(); set => throw new NotSupportedException(); }
    public override void Flush() { }
    public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
    public override void SetLength(long value) => throw new NotSupportedException();
}
