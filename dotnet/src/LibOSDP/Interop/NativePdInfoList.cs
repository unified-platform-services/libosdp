// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

using System.Runtime.InteropServices;
using System.Text;

namespace LibOSDP.Interop;

/// osdp_pd_info_t[] for setup. LibOSDP copies names, keys and capabilities
/// during setup (osdp_cp.c, osdp_pd.c), so this is freed right after.
internal sealed unsafe class NativePdInfoList : IDisposable
{
    private readonly List<(nint Pointer, int Size)> _allocations = [];

    internal NativePdInfo* Pointer { get; }

    internal NativePdInfoList(IReadOnlyList<PdInfo> infos, OsdpFlags forcedFlags,
                              IReadOnlyList<PdCapability>? capabilities)
    {
        // Validate before allocating: a throwing constructor is never disposed.
        foreach (PdInfo info in infos)
            info.Validate();

        NativePdCap* caps = null;
        if (capabilities is not null)
        {
            caps = (NativePdCap*)Alloc(sizeof(NativePdCap) * (capabilities.Count + 1));
            for (int i = 0; i < capabilities.Count; i++)
            {
                caps[i] = new NativePdCap
                {
                    FunctionCode = (int)capabilities[i].FunctionCode,
                    ComplianceLevel = capabilities[i].ComplianceLevel,
                    NumItems = capabilities[i].NumItems,
                };
            }
            caps[capabilities.Count].FunctionCode = NativeLimits.CapSentinel;
        }

        Pointer = (NativePdInfo*)Alloc(sizeof(NativePdInfo) * infos.Count);
        for (int i = 0; i < infos.Count; i++)
        {
            PdInfo info = infos[i];
            Pointer[i] = new NativePdInfo
            {
                Name = CopyOf(Encoding.UTF8.GetBytes((info.Name ?? $"PD-{info.Address}") + '\0')),
                BaudRate = info.BaudRate,
                Address = info.Address,
                Flags = (uint)(info.Flags | forcedFlags),
                Id = info.Id.ToNative(),
                Cap = caps,
                Scbk = info.Scbk is null ? null : CopyOf(info.Scbk),
            };
        }
    }

    private byte* CopyOf(byte[] data)
    {
        var p = (byte*)Alloc(data.Length);
        data.CopyTo(new Span<byte>(p, data.Length));
        return p;
    }

    private void* Alloc(int size)
    {
        void* p = NativeHeap.AllocZeroed((nuint)size);
        _allocations.Add(((nint)p, size));
        return p;
    }

    public void Dispose()
    {
        // Keys pass through here; do not leave them in freed memory.
        foreach (var (pointer, size) in _allocations)
        {
            NativeMemory.Clear((void*)pointer, (nuint)size);
            NativeHeap.Free((void*)pointer);
        }
        _allocations.Clear();
    }
}
