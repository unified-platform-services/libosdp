// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

namespace LibOSDP;

/// <summary>
/// Storage for an OSDP file transfer: the CP side reads the file, the PD side
/// writes it. LibOSDP calls these from its refresh loop, so they must return
/// promptly.
/// </summary>
public interface IFileOperations
{
    /// <summary>Open the pre-agreed file.</summary>
    /// <param name="fileId">Which file.</param>
    /// <param name="size">Sender: set to the file size. Receiver: the incoming size.</param>
    /// <returns>False to refuse the transfer.</returns>
    bool Open(int fileId, ref uint size);

    /// <summary>Read the next chunk (sender).</summary>
    /// <param name="buffer">Where to put it.</param>
    /// <param name="offset">Position in the file.</param>
    /// <returns>
    /// Bytes read, at most <c>buffer.Length</c> (more fails the transfer). 0 means
    /// no data is ready yet: LibOSDP sends a keep-alive and asks again, and
    /// repeated zeros abort. The end of the file is when <paramref name="offset"/>
    /// reaches the size given by <see cref="Open"/>. Negative aborts.
    /// </returns>
    int Read(Span<byte> buffer, uint offset);

    /// <summary>Store a received chunk (receiver).</summary>
    /// <param name="buffer">The chunk.</param>
    /// <param name="offset">Position in the file.</param>
    /// <returns>Bytes written. Anything other than the full chunk aborts the transfer.</returns>
    int Write(ReadOnlySpan<byte> buffer, uint offset);

    /// <summary>The transfer ended. Not called if the device is disposed mid-transfer.</summary>
    void Close();

    /// <summary>
    /// PD only: called after the last byte, and again on each CP keep-alive
    /// while it returns false, so the application can verify or install the
    /// file without holding up the link. LibOSDP aborts the transfer if it is
    /// still false after a fixed timeout (OSDP_FILE_FINALIZE_TIMEOUT_MS).
    /// </summary>
    /// <param name="outcome">How the transfer ends, when returning true.</param>
    /// <param name="delayMs">How long the CP should wait before asking again.</param>
    /// <returns>True when finished.</returns>
    bool TryFinish(out MpOutcome outcome, out ushort delayMs)
    {
        outcome = MpOutcome.Ok;
        delayMs = 0;
        return true;
    }
}
