// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

using System.Runtime.InteropServices;

namespace LibOSDP.Interop;

internal sealed record FileOpsHost(IFileOperations Operations, HandlerErrors Errors);

internal static unsafe class FileOpsBridge
{
    /// LibOSDP copies this struct (osdp_file.c); the GCHandle must stay
    /// allocated until teardown.
    internal static NativeFileOps Describe(GCHandle host) => new()
    {
        Arg = GCHandle.ToIntPtr(host),
        Open = &Open,
        Read = &Read,
        Write = &Write,
        Close = &Close,
        Finalize = &Finish,
    };

    private static FileOpsHost? Host(nint arg)
    {
        try
        {
            return GCHandle.FromIntPtr(arg).Target as FileOpsHost;
        }
        catch (InvalidOperationException)
        {
            return null;
        }
    }

    // LibOSDP trusts the count as a chunk length, so an over-report would make
    // it read past the buffer.
    private static int CheckedCount(FileOpsHost host, string operation, int count, uint capacity)
    {
        if (count <= capacity)
            return count;
        host.Errors.Record(new InvalidOperationException(
            $"{host.Operations.GetType().Name}.{operation} reported {count} bytes for a {capacity}-byte buffer."));
        return -1;
    }

    [UnmanagedCallersOnly]
    private static int Open(nint arg, int fileId, uint* size)
    {
        FileOpsHost? host = Host(arg);
        if (host is null)
            return -1;
        try
        {
            uint value = *size;
            if (!host.Operations.Open(fileId, ref value))
                return -1;
            *size = value;
            return 0;
        }
        catch (Exception e)
        {
            host.Errors.Record(e);
            return -1;
        }
    }

    [UnmanagedCallersOnly]
    private static int Read(nint arg, void* buffer, uint size, uint offset)
    {
        FileOpsHost? host = Host(arg);
        if (host is null)
            return -1;
        try
        {
            int n = host.Operations.Read(new Span<byte>(buffer, checked((int)size)), offset);
            return CheckedCount(host, nameof(IFileOperations.Read), n, size);
        }
        catch (Exception e)
        {
            host.Errors.Record(e);
            return -1;
        }
    }

    [UnmanagedCallersOnly]
    private static int Write(nint arg, void* buffer, uint size, uint offset)
    {
        FileOpsHost? host = Host(arg);
        if (host is null)
            return -1;
        try
        {
            int n = host.Operations.Write(new ReadOnlySpan<byte>(buffer, checked((int)size)), offset);
            return CheckedCount(host, nameof(IFileOperations.Write), n, size);
        }
        catch (Exception e)
        {
            host.Errors.Record(e);
            return -1;
        }
    }

    [UnmanagedCallersOnly]
    private static int Close(nint arg)
    {
        FileOpsHost? host = Host(arg);
        if (host is null)
            return -1;
        try
        {
            host.Operations.Close();
            return 0;
        }
        catch (Exception e)
        {
            host.Errors.Record(e);
            return -1;
        }
    }

    [UnmanagedCallersOnly]
    private static byte Finish(nint arg, int* outcome, ushort* delayMs)
    {
        FileOpsHost? host = Host(arg);
        if (host is null)
        {
            *outcome = (int)MpOutcome.Aborted;
            return 1;
        }
        try
        {
            bool done = host.Operations.TryFinish(out MpOutcome result, out ushort delay);
            *delayMs = delay;
            if (done)
                *outcome = (int)result;
            return done ? (byte)1 : (byte)0;
        }
        catch (Exception e)
        {
            host.Errors.Record(e);
            *outcome = (int)MpOutcome.Aborted;
            return 1;
        }
    }
}
