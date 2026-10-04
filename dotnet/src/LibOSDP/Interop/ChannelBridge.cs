// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

using System.Runtime.InteropServices;

namespace LibOSDP.Interop;

internal sealed record ChannelHost(OsdpChannel Channel, HandlerErrors Errors);

internal static unsafe class ChannelBridge
{
    /// LibOSDP copies this struct at setup; the GCHandle it carries must stay
    /// allocated until teardown.
    internal static NativeChannel Describe(GCHandle host) => new()
    {
        Data = GCHandle.ToIntPtr(host),
        Recv = &Receive,
        Send = &Send,
        Flush = &Flush,
        Close = null, // the caller owns the channel and disposes it
    };

    internal static int ReceiveForTest(GCHandle host, Span<byte> buffer)
    {
        fixed (byte* p = buffer)
            return ReceiveCore(GCHandle.ToIntPtr(host), p, buffer.Length);
    }

    internal static int SendForTest(GCHandle host, ReadOnlySpan<byte> buffer)
    {
        fixed (byte* p = buffer)
            return SendCore(GCHandle.ToIntPtr(host), p, buffer.Length);
    }

    private static ChannelHost? Host(nint data)
    {
        try
        {
            return GCHandle.FromIntPtr(data).Target as ChannelHost;
        }
        catch (InvalidOperationException)
        {
            return null;
        }
    }

    // [UnmanagedCallersOnly] methods cannot be called from managed code, so
    // each is a shell around a core the tests can drive too.
    [UnmanagedCallersOnly]
    private static int Receive(nint data, byte* buffer, int maxLength) => ReceiveCore(data, buffer, maxLength);

    [UnmanagedCallersOnly]
    private static int Send(nint data, byte* buffer, int length) => SendCore(data, buffer, length);

    [UnmanagedCallersOnly]
    private static void Flush(nint data)
    {
        ChannelHost? host = Host(data);
        if (host is null)
            return;
        try
        {
            host.Channel.Flush();
        }
        catch (Exception e)
        {
            host.Errors.Record(e);
        }
    }

    private static int ReceiveCore(nint data, byte* buffer, int maxLength)
    {
        ChannelHost? host = Host(data);
        if (host is null)
            return -1;
        try
        {
            int n = host.Channel.Read(new Span<byte>(buffer, maxLength));
            return CheckedCount(host, n, maxLength, "Read");
        }
        catch (Exception e)
        {
            host.Errors.Record(e);
            return -1;
        }
    }

    private static int SendCore(nint data, byte* buffer, int length)
    {
        ChannelHost? host = Host(data);
        if (host is null)
            return -1;
        try
        {
            int n = host.Channel.Write(new ReadOnlySpan<byte>(buffer, length));
            return CheckedCount(host, n, length, "Write");
        }
        catch (Exception e)
        {
            host.Errors.Record(e);
            return -1;
        }
    }

    private static int CheckedCount(ChannelHost host, int count, int limit, string method)
    {
        if (count <= limit)
            return count;
        host.Errors.Record(new InvalidOperationException(
            $"{host.Channel.GetType().Name}.{method} returned {count}, more than the {limit} bytes it was given."));
        return -1;
    }
}
