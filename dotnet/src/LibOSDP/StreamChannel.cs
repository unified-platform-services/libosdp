// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

using System.Runtime.ExceptionServices;

namespace LibOSDP;

/// <summary>
/// Adapts any <see cref="Stream"/> — <c>SerialPort.BaseStream</c>, a
/// <c>NetworkStream</c> — into an <see cref="OsdpChannel"/>. Stream reads
/// block, which LibOSDP must never do, so a background reader moves incoming
/// bytes into a buffer that <see cref="Read"/> drains without waiting.
/// </summary>
public sealed class StreamChannel : OsdpChannel
{
    private const int MaxBuffered = 64 * 1024;

    private readonly Stream _stream;
    private readonly bool _leaveOpen;
    private readonly Queue<byte> _received = new();
    private readonly CancellationTokenSource _stop = new();
    private readonly Task _pump;
    private bool _ended;
    private Exception? _failure;
    private int _disposed;

    /// <summary>Start reading from <paramref name="stream"/>.</summary>
    /// <param name="stream">A readable, writable stream.</param>
    /// <param name="leaveOpen">Keep the stream open when this channel is disposed.</param>
    public StreamChannel(Stream stream, bool leaveOpen = false)
    {
        ArgumentNullException.ThrowIfNull(stream);
        if (!stream.CanRead || !stream.CanWrite)
            throw new ArgumentException("The stream must be readable and writable.", nameof(stream));
        _stream = stream;
        _leaveOpen = leaveOpen;
        _pump = Task.Run(() => PumpAsync(_stop.Token));
    }

    private async Task PumpAsync(CancellationToken stop)
    {
        byte[] chunk = new byte[512];
        try
        {
            while (true)
            {
                int n = await _stream.ReadAsync(chunk, stop).ConfigureAwait(false);
                if (n == 0)
                    break;
                lock (_received)
                {
                    for (int i = 0; i < n; i++)
                        _received.Enqueue(chunk[i]);
                    // LibOSDP drains this on every refresh; only a stalled bus
                    // overflows it, and the oldest bytes are the ones a
                    // resynchronising framer would discard anyway.
                    while (_received.Count > MaxBuffered)
                        _received.Dequeue();
                }
            }
        }
        catch (OperationCanceledException) when (stop.IsCancellationRequested)
        {
            // Our own Dispose stopped the pump; nothing to report.
        }
        catch (Exception e)
        {
            // A real I/O failure must reach the user, unlike a plain end of
            // stream; Read rethrows it once the buffered bytes are consumed.
            lock (_received)
                _failure = e;
        }
        finally
        {
            lock (_received)
                _ended = true;
        }
    }

    /// <inheritdoc/>
    public override int Read(Span<byte> buffer)
    {
        lock (_received)
        {
            if (_received.Count == 0)
            {
                if (!_ended)
                    return 0;
                Exception? failure = _failure;
                _failure = null;
                if (failure is not null)
                    ExceptionDispatchInfo.Capture(failure).Throw();
                return -1;
            }
            int n = Math.Min(buffer.Length, _received.Count);
            for (int i = 0; i < n; i++)
                buffer[i] = _received.Dequeue();
            return n;
        }
    }

    /// <summary>Writes synchronously, so a stalled stream stalls the caller's refresh loop.</summary>
    /// <param name="buffer">Bytes to send.</param>
    /// <returns>The number of bytes written.</returns>
    public override int Write(ReadOnlySpan<byte> buffer)
    {
        _stream.Write(buffer);
        return buffer.Length;
    }

    /// <inheritdoc/>
    public override void Flush()
    {
        lock (_received)
            _received.Clear();
    }

    /// <inheritdoc/>
    protected override void Dispose(bool disposing)
    {
        if (!disposing || Interlocked.Exchange(ref _disposed, 1) != 0)
            return;
        _stop.Cancel();
        // Some streams (SerialPort on Windows) ignore cancellation; closing
        // the stream is what unblocks their pending read.
        if (!_leaveOpen)
            _stream.Dispose();
        _pump.Wait(TimeSpan.FromSeconds(1));
        _stop.Dispose();
    }
}
