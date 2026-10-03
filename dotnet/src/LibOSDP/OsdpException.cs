// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

namespace LibOSDP;

/// <summary>LibOSDP refused an operation.</summary>
/// <param name="message">What was refused and why.</param>
public class OsdpException(string message) : Exception(message);

/// <summary>
/// The native libosdp next to the application was built with options that
/// change the layout this binding marshals, so calling into it would corrupt
/// memory. Use the libosdp shipped in the LibOSDP package.
/// </summary>
/// <param name="message">Which fields differ.</param>
public sealed class OsdpAbiMismatchException(string message) : OsdpException(message);
