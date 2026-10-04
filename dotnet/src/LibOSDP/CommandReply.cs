// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

namespace LibOSDP;

/// <summary>Decides how a PD answers a command. Runs on the refresh thread; keep it short.</summary>
/// <param name="command">The command the CP sent.</param>
/// <returns>How to answer it.</returns>
public delegate CommandReply CommandHandler(OsdpCommand command);

/// <summary>How a <see cref="PeripheralDevice"/> answers a command.</summary>
public readonly struct CommandReply
{
    private CommandReply(NakCode? nak, OsdpCommand? answer)
    {
        Nak = nak;
        Answer = answer;
    }

    /// <summary>Acknowledge the command (osdp_ACK).</summary>
    public static CommandReply Ack => default;

    /// <summary>Refuse the command (osdp_NAK).</summary>
    /// <param name="code">Why.</param>
    /// <returns>The reply.</returns>
    public static CommandReply Reject(NakCode code = NakCode.Record) => new(code, null);

    /// <summary>
    /// Acknowledge the command and answer with data, e.g. a
    /// <see cref="StatusCommand"/> carrying the requested report. The answer
    /// must be the same kind of command as the one received.
    /// </summary>
    /// <param name="answer">The filled-in command.</param>
    /// <returns>The reply.</returns>
    public static CommandReply WithAnswer(OsdpCommand answer) =>
        new(null, answer ?? throw new ArgumentNullException(nameof(answer)));

    /// <summary>The NAK reason, or null when accepting.</summary>
    public NakCode? Nak { get; }

    /// <summary>The answer data, or null.</summary>
    public OsdpCommand? Answer { get; }

    internal int NativeResult => Nak is { } code ? -(int)code : 0;
}
