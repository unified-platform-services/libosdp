// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

using System.Runtime.InteropServices;
using LibOSDP.Interop;

namespace LibOSDP;

/// <summary>Library-wide information and settings.</summary>
public static unsafe class Osdp
{
    private static readonly object s_initGate = new();
    private static volatile bool s_initialized;

    /// <summary>
    /// How often <c>ControlPanel.RunAsync</c> and
    /// <c>PeripheralDevice.RunAsync</c> service the bus. OSDP requires
    /// at least once every 50 ms.
    /// </summary>
    public static TimeSpan RefreshInterval { get; } = TimeSpan.FromMilliseconds(20);

    /// <summary>Version of the native libosdp in use, e.g. <c>4.0.0</c>.</summary>
    public static string Version
    {
        get
        {
            EnsureInitialized();
            return Marshal.PtrToStringUTF8((nint)Native.GetVersion()) ?? "";
        }
    }

    internal static void EnsureInitialized()
    {
        if (s_initialized)
            return;
        lock (s_initGate)
        {
            if (s_initialized)
                return;
            AbiGuard.Verify(AbiGuard.Query(), AbiGuard.Expected());
            s_initialized = true;
        }
    }
}
