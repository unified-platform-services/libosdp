// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

using System.Text;

namespace LibOSDP.Interop;

internal static unsafe class Bytes
{
    /// Copies src into a fixed native buffer, refusing anything that does not fit.
    internal static int Fill(byte[] src, byte* dst, int capacity, string paramName)
    {
        ArgumentNullException.ThrowIfNull(src, paramName);
        if (src.Length > capacity)
            throw new ArgumentException($"{paramName} is {src.Length} bytes; at most {capacity} fit.", paramName);
        src.CopyTo(new Span<byte>(dst, capacity));
        return src.Length;
    }

    /// Copies out of a fixed native buffer, trusting the length only as far as the buffer goes.
    internal static byte[] Read(byte* src, int length, int capacity) =>
        new ReadOnlySpan<byte>(src, Math.Clamp(length, 0, capacity)).ToArray();

    internal static byte[] Ascii(string text, string paramName)
    {
        ArgumentNullException.ThrowIfNull(text, paramName);
        foreach (char c in text)
        {
            if (c > 0x7F)
                throw new ArgumentException($"{paramName} must be ASCII.", paramName);
        }
        return Encoding.ASCII.GetBytes(text);
    }
}
