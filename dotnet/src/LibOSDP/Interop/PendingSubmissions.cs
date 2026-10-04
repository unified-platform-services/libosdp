// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

using System.Collections.Concurrent;
using System.Runtime.InteropServices;

namespace LibOSDP.Interop;

/// Native buffers LibOSDP holds by reference until it reports their completion.
internal sealed unsafe class PendingSubmissions
{
    private sealed record Entry(int Size, TaskCompletionSource<CompletionStatus> Completion);

    private readonly ConcurrentDictionary<nint, Entry> _pending = new();

    internal Task<CompletionStatus> Track(void* buffer, int size)
    {
        // Completions run inside refresh while the device lock is held;
        // continuations must not run there.
        var completion = new TaskCompletionSource<CompletionStatus>(TaskCreationOptions.RunContinuationsAsynchronously);
        _pending[(nint)buffer] = new Entry(size, completion);
        return completion.Task;
    }

    /// The submit was refused, so LibOSDP never took the buffer. A Dispose
    /// racing the submit may already have aborted and freed it.
    internal void Discard(void* buffer)
    {
        if (_pending.TryRemove((nint)buffer, out Entry? entry))
            Free(buffer, entry.Size);
    }

    /// Ignores buffers LibOSDP allocated itself.
    internal void Complete(void* buffer, CompletionStatus status)
    {
        if (_pending.TryRemove((nint)buffer, out Entry? entry))
        {
            Free(buffer, entry.Size);
            entry.Completion.TrySetResult(status);
        }
    }

    // KEYSET, GENAUTH and CRAUTH carry key material; do not leave it in freed memory.
    private static void Free(void* buffer, int size)
    {
        NativeMemory.Clear(buffer, (nuint)size);
        NativeHeap.Free(buffer);
    }

    /// After teardown nothing references the buffers any more.
    internal void AbortRemaining()
    {
        foreach (nint buffer in _pending.Keys)
            Complete((void*)buffer, CompletionStatus.Aborted);
    }
}
