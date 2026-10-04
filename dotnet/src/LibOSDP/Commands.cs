// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

namespace LibOSDP;

/// <summary>
/// A command a control panel sends to a peripheral device. Values are
/// validated when the command is submitted.
/// </summary>
public abstract record OsdpCommand
{
    private protected OsdpCommand() { }
}

/// <summary>Drive an output (osdp_OUT).</summary>
/// <param name="OutputNo">Output number, from 0.</param>
/// <param name="ControlCode">What to do.</param>
/// <param name="TimerCount">Duration of a temporary state, in units of 100 ms.</param>
public sealed record OutputCommand(byte OutputNo, OutputControlCode ControlCode, ushort TimerCount = 0) : OsdpCommand;

/// <summary>A temporary LED state that runs for a while and then reverts.</summary>
/// <param name="ControlCode">Set or cancel the temporary state.</param>
/// <param name="OnCount">On time, in units of 100 ms.</param>
/// <param name="OffCount">Off time, in units of 100 ms.</param>
/// <param name="OnColor">Colour during the on time.</param>
/// <param name="OffColor">Colour during the off time.</param>
/// <param name="TimerCount">How long the temporary state lasts, in units of 100 ms.</param>
public sealed record TemporaryLedParams(TemporaryLedControlCode ControlCode, byte OnCount, byte OffCount,
                                        LedColor OnColor, LedColor OffColor, ushort TimerCount);

/// <summary>A permanent LED state that holds until changed.</summary>
/// <param name="ControlCode">Set the permanent state.</param>
/// <param name="OnCount">On time, in units of 100 ms.</param>
/// <param name="OffCount">Off time, in units of 100 ms.</param>
/// <param name="OnColor">Colour during the on time.</param>
/// <param name="OffColor">Colour during the off time.</param>
public sealed record PermanentLedParams(PermanentLedControlCode ControlCode, byte OnCount, byte OffCount,
                                        LedColor OnColor, LedColor OffColor);

/// <summary>Drive a reader LED (osdp_LED). A null block leaves that state unchanged.</summary>
/// <param name="Reader">Reader number, from 0.</param>
/// <param name="LedNumber">LED number on that reader.</param>
/// <param name="Temporary">Temporary state, or null for no change.</param>
/// <param name="Permanent">Permanent state, or null for no change.</param>
public sealed record LedCommand(byte Reader, byte LedNumber, TemporaryLedParams? Temporary = null,
                                PermanentLedParams? Permanent = null) : OsdpCommand;

/// <summary>Drive a reader buzzer (osdp_BUZ).</summary>
/// <param name="Reader">Reader number, from 0.</param>
/// <param name="ControlCode">Tone.</param>
/// <param name="OnCount">On time, in units of 100 ms.</param>
/// <param name="OffCount">Off time, in units of 100 ms.</param>
/// <param name="RepCount">Repetitions; 0 repeats until changed.</param>
public sealed record BuzzerCommand(byte Reader, BuzzerControlCode ControlCode, byte OnCount, byte OffCount,
                                   byte RepCount) : OsdpCommand;

/// <summary>Show text on a reader display (osdp_TEXT).</summary>
/// <param name="Reader">Reader number, from 0.</param>
/// <param name="ControlCode">Permanent or temporary, wrapped or not.</param>
/// <param name="TempTime">Duration of temporary text, in seconds.</param>
/// <param name="OffsetRow">Starting row, from 1.</param>
/// <param name="OffsetCol">Starting column, from 1.</param>
/// <param name="Data">ASCII text, at most 32 characters.</param>
public sealed record TextCommand(byte Reader, TextControlCode ControlCode, byte TempTime, byte OffsetRow,
                                 byte OffsetCol, string Data) : OsdpCommand;

/// <summary>Set a new secure channel base key (osdp_KEYSET). Requires an active secure channel.</summary>
/// <param name="Type">Key type; 1 is the SCBK.</param>
/// <param name="Data">The key, at most 32 bytes (16 for an SCBK).</param>
public sealed record KeysetCommand(byte Type, byte[] Data) : OsdpCommand;

/// <summary>Change the PD's address and baud rate (osdp_COMSET).</summary>
/// <param name="Address">New address.</param>
/// <param name="BaudRate">New baud rate.</param>
public sealed record ComsetCommand(byte Address, uint BaudRate) : OsdpCommand;

/// <summary>Delivered to a PD after a <see cref="ComsetCommand"/> has been answered.</summary>
/// <param name="Address">Address now in use.</param>
/// <param name="BaudRate">Baud rate now in use.</param>
public sealed record ComsetDoneCommand(byte Address, uint BaudRate) : OsdpCommand;

/// <summary>Manufacturer-specific command (osdp_MFG).</summary>
/// <param name="VendorCode">IEEE OUI of the vendor (low 24 bits).</param>
/// <param name="Data">Payload, at most 64 bytes.</param>
public sealed record ManufacturerCommand(uint VendorCode, byte[] Data) : OsdpCommand;

/// <summary>
/// Send a file to the PD (osdp_FILETRANSFER). Both sides need file operations
/// registered first; the command completes when the transfer ends.
/// </summary>
/// <param name="FileId">Pre-agreed file identifier.</param>
/// <param name="Flags">Transfer flags; 0 for none.</param>
public sealed record FileTransferCommand(int FileId, uint Flags = 0) : OsdpCommand;

/// <summary>
/// Ask the PD for a status report (osdp_ISTAT, OSTAT, LSTAT, RSTAT). A PD answers
/// with <see cref="CommandReply.WithAnswer"/> carrying the filled report.
/// </summary>
/// <param name="Type">Which report.</param>
/// <param name="Report">Empty in a query; one byte per entry in an answer, at most 64.</param>
public sealed record StatusCommand(StatusReportType Type, byte[] Report) : OsdpCommand;

/// <summary>A library notification delivered to a PD application. Cannot be submitted.</summary>
/// <param name="Notification">What happened.</param>
public sealed record NotificationCommand(OsdpNotification Notification) : OsdpCommand;

/// <summary>Request a biometric read (osdp_BIOREAD).</summary>
/// <param name="Reader">Reader number, from 0.</param>
/// <param name="Type">Biometric type.</param>
/// <param name="Format">Template format.</param>
/// <param name="Quality">Minimum quality, 0-100.</param>
public sealed record BioReadCommand(byte Reader, BioType Type, BioFormat Format, byte Quality) : OsdpCommand;

/// <summary>Request a biometric match against a template (osdp_BIOMATCH).</summary>
/// <param name="Reader">Reader number, from 0.</param>
/// <param name="Type">Biometric type.</param>
/// <param name="Format">Template format.</param>
/// <param name="Quality">Minimum quality, 0-100.</param>
/// <param name="Template">Template to match, at most 128 bytes.</param>
public sealed record BioMatchCommand(byte Reader, BioType Type, BioFormat Format, byte Quality,
                                     byte[] Template) : OsdpCommand;

/// <summary>Set the PD's clock (osdp_TDSET).</summary>
/// <param name="Year">Year.</param>
/// <param name="Month">Month, 1-12.</param>
/// <param name="Day">Day, 1-31.</param>
/// <param name="Hour">Hour, 0-23.</param>
/// <param name="Minute">Minute, 0-59.</param>
/// <param name="Second">Second, 0-59.</param>
public sealed record TdSetCommand(ushort Year, byte Month, byte Day, byte Hour, byte Minute,
                                  byte Second) : OsdpCommand;

/// <summary>Read a PIV data object from a card (osdp_PIVDATA).</summary>
/// <param name="ObjectId">Three-byte PIV object identifier.</param>
/// <param name="Element">Element within the object.</param>
/// <param name="Offset">Data offset within the element.</param>
public sealed record PivDataCommand(byte[] ObjectId, byte Element, byte Offset) : OsdpCommand;

/// <summary>PIV general authenticate (osdp_GENAUTH).</summary>
/// <param name="Algorithm">Algorithm reference.</param>
/// <param name="Key">Key reference.</param>
/// <param name="Data">Authentication template, at most 256 bytes.</param>
public sealed record GenAuthCommand(byte Algorithm, byte Key, byte[] Data) : OsdpCommand;

/// <summary>PIV challenge/response authenticate (osdp_CRAUTH).</summary>
/// <param name="Algorithm">Algorithm reference.</param>
/// <param name="Key">Key reference.</param>
/// <param name="Data">Challenge, at most 256 bytes.</param>
public sealed record CrAuthCommand(byte Algorithm, byte Key, byte[] Data) : OsdpCommand;
