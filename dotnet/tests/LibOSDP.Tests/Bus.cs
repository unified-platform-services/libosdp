// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

using System.Security.Cryptography;
using System.Threading.Channels;

namespace LibOSDP.Tests;

internal sealed record BusOptions
{
    public byte[]? CpKey { get; init; }
    public byte[]? PdKey { get; init; }
    public OsdpFlags CpFlags { get; init; }
    public OsdpFlags PdFlags { get; init; }
    public PdId PdId { get; init; } = new();
    public IReadOnlyList<PdCapability> Capabilities { get; init; } = Bus.DefaultCapabilities;

    public static BusOptions Secure()
    {
        byte[] key = RandomNumberGenerator.GetBytes(16);
        return new() { CpKey = key, PdKey = key, CpFlags = OsdpFlags.EnforceSecure, PdFlags = OsdpFlags.EnforceSecure };
    }
}

/// A CP and a PD talking over a loopback pair, each refreshed in the background.
internal sealed class Bus : IAsyncDisposable
{
    internal const int Address = 101;

    internal static readonly TimeSpan Timeout = TestTime.Timeout;

    internal static readonly PdCapability[] DefaultCapabilities =
    [
        new(Capability.OutputControl, 1, 8),
        new(Capability.ContactStatusMonitoring, 1, 8),
        new(Capability.LedControl, 1, 1),
        new(Capability.AudibleControl, 1, 1),
        new(Capability.TextOutput, 1, 1),
        new(Capability.Readers, 1, 2),
        new(Capability.Biometrics, 1, 1),
        new(Capability.TimeKeeping, 1, 0),
    ];

    private readonly CancellationTokenSource _stop = new();
    private readonly Channel<OsdpCommand> _commands = Channel.CreateUnbounded<OsdpCommand>();
    private readonly Channel<OsdpEvent> _events = Channel.CreateUnbounded<OsdpEvent>();

    public ControlPanel Cp { get; }
    public PeripheralDevice Pd { get; }
    public Task CpLoop { get; }
    public Task PdLoop { get; }
    public Func<OsdpCommand, CommandReply> Reply { get; set; } = _ => CommandReply.Ack;
    public bool LoopFaultExpected { get; set; }

    private Bus(BusOptions o)
    {
        var (cpSide, pdSide) = LoopbackChannel.CreatePair();
        ControlPanel? cp = null;
        try
        {
            cp = new ControlPanel(cpSide, [new PdInfo(Address) { Scbk = o.CpKey, Flags = o.CpFlags }]);
            Pd = new PeripheralDevice(pdSide, new PdInfo(Address) { Scbk = o.PdKey, Flags = o.PdFlags, Id = o.PdId },
                                      o.Capabilities);
        }
        catch
        {
            cp?.Dispose();
            cpSide.Dispose();
            pdSide.Dispose();
            throw;
        }
        Cp = cp;
        Pd.CommandHandler = command =>
        {
            _commands.Writer.TryWrite(command);
            return Reply(command);
        };
        Cp.EventReceived += (_, e) => _events.Writer.TryWrite(e.Event);
        CpLoop = Task.Run(() => Cp.RunAsync(_stop.Token));
        PdLoop = Task.Run(() => Pd.RunAsync(_stop.Token));
    }

    public static async Task<Bus> StartAsync(BusOptions? options = null)
    {
        options ??= new BusOptions();
        var bus = new Bus(options);
        bool up = options.CpKey is null
            ? await bus.Cp.WaitOnlineAsync(Address, Timeout)
            : await bus.Cp.WaitSecureChannelAsync(Address, Timeout);
        if (!up)
        {
            await bus.DisposeAsync();
            throw new TimeoutException("The PD did not come up.");
        }
        return bus;
    }

    public async Task<T> NextCommandAsync<T>() where T : OsdpCommand
    {
        using var timeout = new CancellationTokenSource(Timeout);
        while (true)
        {
            if (await _commands.Reader.ReadAsync(timeout.Token) is T match)
                return match;
        }
    }

    public async Task<OsdpEvent> NextEventAsync(Func<OsdpEvent, bool> wanted)
    {
        using var timeout = new CancellationTokenSource(Timeout);
        while (true)
        {
            OsdpEvent ev = await _events.Reader.ReadAsync(timeout.Token);
            if (wanted(ev))
                return ev;
        }
    }

    public async ValueTask DisposeAsync()
    {
        _stop.Cancel();
        foreach (Task loop in new[] { CpLoop, PdLoop })
        {
            try
            {
                await loop;
            }
            catch (OperationCanceledException)
            {
            }
            catch (AggregateException) when (LoopFaultExpected)
            {
            }
        }
        Cp.Dispose();
        Pd.Dispose();
        _stop.Dispose();
    }
}
