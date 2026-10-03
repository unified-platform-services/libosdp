// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

namespace LibOSDP;

/// <summary>
/// Something a peripheral device reports to its control panel. Values are
/// validated when the event is submitted.
/// </summary>
public abstract record OsdpEvent
{
    private protected OsdpEvent() { }
}

/// <summary>A card was read (osdp_RAW).</summary>
/// <param name="ReaderNo">Reader number, from 0.</param>
/// <param name="Format">
/// Raw bits. LibOSDP does not support <see cref="CardFormat.Ascii"/> (osdp_FMT);
/// submitting it throws <see cref="ArgumentException"/>.
/// </param>
/// <param name="Data">Card data, at most 64 bytes. Raw formats pad the last byte.</param>
/// <param name="Bits">
/// Length in bits for raw formats; null means every bit of <paramref name="Data"/>.
/// Null on received <see cref="CardFormat.Ascii"/> events; always set on received
/// events with a raw format.
/// </param>
public sealed record CardReadEvent(int ReaderNo, CardFormat Format, byte[] Data, int? Bits = null) : OsdpEvent;

/// <summary>Keys were pressed on a reader keypad (osdp_KEYPAD).</summary>
/// <param name="ReaderNo">Reader number, from 0.</param>
/// <param name="Data">Key codes, at most 64.</param>
public sealed record KeyPressEvent(int ReaderNo, byte[] Data) : OsdpEvent;

/// <summary>Manufacturer-specific reply (osdp_MFGREP).</summary>
/// <param name="VendorCode">IEEE OUI of the vendor (low 24 bits).</param>
/// <param name="Data">Payload, at most 128 bytes.</param>
public sealed record ManufacturerReplyEvent(uint VendorCode, byte[] Data) : OsdpEvent;

/// <summary>Manufacturer-specific status (osdp_MFGSTATR).</summary>
/// <param name="Data">Payload, at most 128 bytes.</param>
public sealed record ManufacturerStatusEvent(byte[] Data) : OsdpEvent;

/// <summary>Manufacturer-specific error (osdp_MFGERRR).</summary>
/// <param name="Data">Payload, at most 128 bytes.</param>
public sealed record ManufacturerErrorEvent(byte[] Data) : OsdpEvent;

/// <summary>Result of a biometric read (osdp_BIOREADR).</summary>
/// <param name="Reader">Reader number, from 0.</param>
/// <param name="Status">Outcome.</param>
/// <param name="Type">Biometric type.</param>
/// <param name="Quality">Quality, 0-100.</param>
/// <param name="Template">Template, at most 256 bytes.</param>
public sealed record BioReadEvent(byte Reader, BioStatus Status, BioType Type, byte Quality,
                                  byte[] Template) : OsdpEvent;

/// <summary>Result of a biometric match (osdp_BIOMATCHR).</summary>
/// <param name="Reader">Reader number, from 0.</param>
/// <param name="Status">Outcome.</param>
/// <param name="Score">Match score, 0-100.</param>
public sealed record BioMatchEvent(byte Reader, BioStatus Status, byte Score) : OsdpEvent;

/// <summary>A status report (osdp_ISTATR, OSTATR, LSTATR, RSTATR).</summary>
/// <param name="Type">Which report.</param>
/// <param name="Report">One byte per entry, at most 64.</param>
public sealed record StatusEvent(StatusReportType Type, byte[] Report) : OsdpEvent;

/// <summary>A library notification delivered to a CP application. Cannot be submitted.</summary>
/// <param name="Notification">What happened.</param>
public sealed record NotificationEvent(OsdpNotification Notification) : OsdpEvent;

/// <summary>PIV data read from a card (osdp_PIVDATAR).</summary>
/// <param name="Data">Data, at most 256 bytes.</param>
public sealed record PivDataEvent(byte[] Data) : OsdpEvent;

/// <summary>PIV general authenticate response (osdp_GENAUTHR).</summary>
/// <param name="Data">Response, at most 256 bytes.</param>
public sealed record GenAuthEvent(byte[] Data) : OsdpEvent;

/// <summary>PIV challenge/response result (osdp_CRAUTHR).</summary>
/// <param name="Data">Response, at most 256 bytes.</param>
public sealed record CrAuthEvent(byte[] Data) : OsdpEvent;
