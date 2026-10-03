// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

namespace LibOSDP.Interop;

internal static unsafe class AbiGuard
{
    internal static AbiInfo Query()
    {
        AbiInfo info = default;
        info.Size = (uint)sizeof(AbiInfo);
        try
        {
            Native.GetAbiInfo(&info);
        }
        catch (EntryPointNotFoundException)
        {
            throw new OsdpAbiMismatchException("libosdp too old: no osdp_get_abi_info");
        }
        return info;
    }

    internal static AbiInfo Expected() => new()
    {
        Size = (uint)sizeof(AbiInfo),
        Options = 0,
        SizeofCmd = (uint)sizeof(NativeCmd),
        SizeofEvent = (uint)sizeof(NativeEvent),
        SizeofChannel = (uint)sizeof(NativeChannel),
        SizeofPdInfo = (uint)sizeof(NativePdInfo),
    };

    internal static void Verify(AbiInfo actual, AbiInfo expected)
    {
        var problems = new List<string>();
        if (actual.Size < expected.Size)
            problems.Add($"library reports {actual.Size} bytes of ABI info, binding needs {expected.Size}");
        Compare(problems, "options", actual.Options, expected.Options);
        Compare(problems, "sizeof_cmd", actual.SizeofCmd, expected.SizeofCmd);
        Compare(problems, "sizeof_event", actual.SizeofEvent, expected.SizeofEvent);
        Compare(problems, "sizeof_channel", actual.SizeofChannel, expected.SizeofChannel);
        Compare(problems, "sizeof_pd_info", actual.SizeofPdInfo, expected.SizeofPdInfo);
        if (problems.Count > 0)
            throw new OsdpAbiMismatchException(
                "The loaded libosdp was built with options this binding does not support: " +
                string.Join("; ", problems));
    }

    private static void Compare(List<string> problems, string field, uint actual, uint expected)
    {
        if (actual != expected)
            problems.Add($"{field} is {actual}, expected {expected}");
    }
}
