// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

using System.Runtime.InteropServices;

namespace LibOSDP.Interop;

/// Every native allocation the binding makes goes through here, so tests can
/// prove that teardown hands every buffer back.
internal static unsafe class NativeHeap
{
    private static long s_live;

    internal static long Live => Interlocked.Read(ref s_live);

    internal static void* AllocZeroed(nuint size)
    {
        void* p = NativeMemory.AllocZeroed(size);
        Interlocked.Increment(ref s_live);
        return p;
    }

    internal static void Free(void* p)
    {
        NativeMemory.Free(p);
        Interlocked.Decrement(ref s_live);
    }
}
