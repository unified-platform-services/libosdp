// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

using System.Diagnostics;
using System.Runtime.InteropServices;
using LibOSDP.Interop;

namespace LibOSDP.Tests;

public sealed class ChannelTests
{
    private static readonly TimeSpan PollWindow = TestTime.Scaled(TimeSpan.FromSeconds(2));

    private static int ReadWithin(OsdpChannel channel, Span<byte> buffer, TimeSpan timeout)
    {
        var clock = Stopwatch.StartNew();
        while (clock.Elapsed < timeout)
        {
            int n = channel.Read(buffer);
            if (n != 0)
                return n;
            Thread.Sleep(5);
        }
        return 0;
    }

    [Fact]
    public void LoopbackDeliversToThePeerOnly()
    {
        var (a, b) = LoopbackChannel.CreatePair();
        Assert.Equal(3, a.Write([1, 2, 3]));
        var buffer = new byte[8];
        Assert.Equal(0, a.Read(buffer));
        Assert.Equal(3, b.Read(buffer));
        Assert.Equal(new byte[] { 1, 2, 3 }, buffer[..3]);
    }

    [Fact]
    public void LoopbackFlushDropsPendingInput()
    {
        var (a, b) = LoopbackChannel.CreatePair();
        a.Write([1, 2, 3]);
        b.Flush();
        Assert.Equal(0, b.Read(new byte[8]));
    }

    [Fact]
    public void StalledStreamReadsReturnImmediately()
    {
        using var stream = new ScriptedStream();
        using var channel = new StreamChannel(stream);
        Assert.Equal(0, channel.Read(new byte[16])); // warm-up: JIT and the first pending read
        TimeSpan bound = TestTime.Scaled(TimeSpan.FromMilliseconds(50));
        var clock = Stopwatch.StartNew();
        for (int i = 0; i < 10; i++)
            Assert.Equal(0, channel.Read(new byte[16]));
        Assert.True(clock.Elapsed < bound, $"10 reads took {clock.ElapsedMilliseconds} ms");
    }

    [Fact]
    public void StreamDataReachesRead()
    {
        using var stream = new ScriptedStream();
        using var channel = new StreamChannel(stream);
        stream.Feed(7, 8, 9);
        var buffer = new byte[16];
        Assert.Equal(3, ReadWithin(channel, buffer, PollWindow));
        Assert.Equal(new byte[] { 7, 8, 9 }, buffer[..3]);
    }

    [Fact]
    public void EndedStreamReadsReportErrorAfterDraining()
    {
        using var stream = new ScriptedStream();
        using var channel = new StreamChannel(stream);
        stream.Feed(1);
        stream.End();
        var buffer = new byte[16];
        Assert.Equal(1, ReadWithin(channel, buffer, PollWindow));
        Assert.Equal(-1, ReadWithin(channel, buffer, PollWindow));
    }

    [Fact]
    public void WritesGoStraightToTheStream()
    {
        using var stream = new ScriptedStream();
        using var channel = new StreamChannel(stream);
        Assert.Equal(2, channel.Write([0xFF, 0x53]));
        Assert.Equal(new byte[] { 0xFF, 0x53 }, stream.Written);
    }

    [Fact]
    public void DisposeClosesTheStreamUnlessLeftOpen()
    {
        var owned = new ScriptedStream();
        new StreamChannel(owned).Dispose();
        Assert.True(owned.Disposed);

        var shared = new ScriptedStream();
        new StreamChannel(shared, leaveOpen: true).Dispose();
        Assert.False(shared.Disposed);
        shared.Dispose();
    }

    private sealed class ThrowingChannel : OsdpChannel
    {
        public override int Read(Span<byte> buffer) => throw new IOException("unplugged");
        public override int Write(ReadOnlySpan<byte> buffer) => throw new IOException("unplugged");
    }

    [Fact]
    public void ChannelExceptionBecomesAnErrorCodeAndIsKept()
    {
        var errors = new HandlerErrors();
        GCHandle host = GCHandle.Alloc(new ChannelHost(new ThrowingChannel(), errors));
        try
        {
            Assert.Equal(-1, ChannelBridge.ReceiveForTest(host, new byte[4]));
            Assert.Equal(-1, ChannelBridge.SendForTest(host, new byte[4]));
            var thrown = Assert.Throws<AggregateException>(errors.ThrowIfAny);
            Assert.Equal(2, thrown.InnerExceptions.Count);
            errors.ThrowIfAny(); // cleared once reported
        }
        finally
        {
            host.Free();
        }
    }

    private sealed class FailingStream : Stream
    {
        public override ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default) =>
            throw new IOException("unplugged");
        public override int Read(byte[] buffer, int offset, int count) => throw new IOException("unplugged");
        public override void Write(byte[] buffer, int offset, int count) { }
        public override bool CanRead => true;
        public override bool CanWrite => true;
        public override bool CanSeek => false;
        public override long Length => throw new NotSupportedException();
        public override long Position { get => throw new NotSupportedException(); set => throw new NotSupportedException(); }
        public override void Flush() { }
        public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
        public override void SetLength(long value) => throw new NotSupportedException();
    }

    [Fact]
    public void DoubleDisposeIsHarmless()
    {
        var channel = new StreamChannel(new ScriptedStream());
        channel.Dispose();
        channel.Dispose();
    }

    [Fact]
    public void StreamFailureIsThrownOnceThenReportsError()
    {
        using var channel = new StreamChannel(new FailingStream());
        var buffer = new byte[16];
        var clock = Stopwatch.StartNew();
        IOException? thrown = null;
        while (thrown is null && clock.Elapsed < PollWindow)
        {
            try
            {
                channel.Read(buffer);
            }
            catch (IOException e)
            {
                thrown = e;
            }
            Thread.Sleep(5);
        }
        Assert.NotNull(thrown);
        Assert.Equal("unplugged", thrown.Message);
        Assert.Equal(-1, channel.Read(buffer));
    }

    [Fact]
    public void PlainEndOfStreamReportsErrorWithoutThrowing()
    {
        using var stream = new ScriptedStream();
        using var channel = new StreamChannel(stream);
        stream.End();
        Assert.Equal(-1, ReadWithin(channel, new byte[16], PollWindow));
    }

    private sealed class OverreportingChannel : OsdpChannel
    {
        public override int Read(Span<byte> buffer) => buffer.Length + 1;
        public override int Write(ReadOnlySpan<byte> buffer) => buffer.Length + 1;
    }

    [Fact]
    public void OverreportedCountIsAnError()
    {
        var errors = new HandlerErrors();
        GCHandle host = GCHandle.Alloc(new ChannelHost(new OverreportingChannel(), errors));
        try
        {
            Assert.Equal(-1, ChannelBridge.ReceiveForTest(host, new byte[4]));
            Assert.Equal(-1, ChannelBridge.SendForTest(host, new byte[4]));
            var thrown = Assert.Throws<AggregateException>(errors.ThrowIfAny);
            Assert.All(thrown.InnerExceptions, e => Assert.IsType<InvalidOperationException>(e));
        }
        finally
        {
            host.Free();
        }
    }
}
