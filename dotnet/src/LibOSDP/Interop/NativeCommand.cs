// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

using System.Runtime.InteropServices;

namespace LibOSDP.Interop;

[StructLayout(LayoutKind.Sequential)]
internal struct NativeCmdOutput
{
    public byte OutputNo;
    public int ControlCode;
    public ushort TimerCount;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeCmdLedParams
{
    public byte ControlCode;
    public byte OnCount;
    public byte OffCount;
    public int OnColor;
    public int OffColor;
    public ushort TimerCount;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeCmdLed
{
    public byte Reader;
    public byte LedNumber;
    public NativeCmdLedParams Temporary;
    public NativeCmdLedParams Permanent;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeCmdBuzzer
{
    public byte Reader;
    public int ControlCode;
    public byte OnCount;
    public byte OffCount;
    public byte RepCount;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeCmdText
{
    public byte Reader;
    public int ControlCode;
    public byte TempTime;
    public byte OffsetRow;
    public byte OffsetCol;
    public byte Length;
    public fixed byte Data[NativeLimits.CmdTextMaxLen];
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeCmdComset
{
    public byte Address;
    public uint BaudRate;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeCmdKeyset
{
    public byte Type;
    public byte Length;
    public fixed byte Data[NativeLimits.CmdKeysetKeyMaxLen];
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeCmdMfg
{
    public uint VendorCode;
    public fixed byte Data[NativeLimits.CmdMfgMaxDataLen];
    public byte Length;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeCmdFileTx
{
    public int Id;
    public uint Flags;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeCmdBioRead
{
    public byte Reader;
    public int Type;
    public int Format;
    public byte Quality;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeCmdBioMatch
{
    public byte Reader;
    public int Type;
    public int Format;
    public byte Quality;
    public ushort Length;
    public fixed byte Data[NativeLimits.CmdBiomatchMaxTemplateLen];
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeCmdTdSet
{
    public ushort Year;
    public byte Month;
    public byte Day;
    public byte Hour;
    public byte Minute;
    public byte Second;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeCmdPivData
{
    public fixed byte Oid[3];
    public byte Element;
    public byte Offset;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeCmdAuth
{
    public byte Algorithm;
    public byte Key;
    public ushort Length;
    public fixed byte Data[NativeLimits.PivDataMaxLen];
}

[StructLayout(LayoutKind.Explicit, Size = NativeLimits.CmdUnionSize)]
internal struct NativeCmdUnion
{
    [FieldOffset(0)] public NativeCmdLed Led;
    [FieldOffset(0)] public NativeCmdBuzzer Buzzer;
    [FieldOffset(0)] public NativeCmdText Text;
    [FieldOffset(0)] public NativeCmdOutput Output;
    [FieldOffset(0)] public NativeCmdComset Comset;
    [FieldOffset(0)] public NativeCmdKeyset Keyset;
    [FieldOffset(0)] public NativeCmdMfg Mfg;
    [FieldOffset(0)] public NativeCmdFileTx FileTx;
    [FieldOffset(0)] public NativeStatusReport Status;
    [FieldOffset(0)] public NativeNotification Notif;
    [FieldOffset(0)] public NativeCmdBioRead BioRead;
    [FieldOffset(0)] public NativeCmdBioMatch BioMatch;
    [FieldOffset(0)] public NativeCmdTdSet TdSet;
    [FieldOffset(0)] public NativeCmdPivData PivData;
    [FieldOffset(0)] public NativeCmdAuth Auth;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeCmd
{
    public NativeQueueNode Node;
    public int Id;
    public uint Flags;
    public NativeCmdUnion U;
}
