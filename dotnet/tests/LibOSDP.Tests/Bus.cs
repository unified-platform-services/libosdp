// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Threading.Channels;

namespace LibOSDP.Tests;

/// How the CP and the PD of a <see cref="Bus"/> reach each other.
public enum Transport
{
    Loopback,
    /// Two StreamChannels over the ends of a localhost TCP connection.
    Tcp,
}

internal sealed record BusOptions
{
    public byte[]? CpKey { get; init; }
    public byte[]? PdKey { get; init; }
    public OsdpFlags CpFlags { get; init; }
    public OsdpFlags PdFlags { get; init; }
    public PdId PdId { get; init; } = new();
    public IReadOnlyList<PdCapability> Capabilities { get; init; } = Bus.DefaultCapabilities;
    public Transport Transport { get; init; }

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
    private readonly OsdpChannel _cpSide;
    private readonly OsdpChannel _pdSide;

    public ControlPanel Cp { get; }
    public PeripheralDevice Pd { get; }
    public Task CpLoop { get; }
    public Task PdLoop { get; }
    public Func<OsdpCommand, CommandReply> Reply { get; set; } = _ => CommandReply.Ack;
    public bool LoopFaultExpected { get; set; }

    private Bus(BusOptions o)
    {
        (_cpSide, _pdSide) = CreateLink(o.Transport);
        ControlPanel? cp = null;
        try
        {
            cp = new ControlPanel(_cpSide, [new PdInfo(Address) { Scbk = o.CpKey, Flags = o.CpFlags }]);
            Pd = new PeripheralDevice(_pdSide, new PdInfo(Address) { Scbk = o.PdKey, Flags = o.PdFlags, Id = o.PdId },
                                      o.Capabilities);
        }
        catch
        {
            cp?.Dispose();
            _cpSide.Dispose();
            _pdSide.Dispose();
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

    private static (OsdpChannel Cp, OsdpChannel Pd) CreateLink(Transport transport)
    {
        if (transport == Transport.Loopback)
            return LoopbackChannel.CreatePair();
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var client = new TcpClient { NoDelay = true };
        Socket? server = null;
        StreamChannel? cpSide = null;
        try
        {
            client.Connect((IPEndPoint)listener.LocalEndpoint);
            server = listener.AcceptSocket();
            server.NoDelay = true;
            cpSide = new StreamChannel(client.GetStream());
            return (cpSide, new StreamChannel(new NetworkStream(server, ownsSocket: true)));
        }
        catch
        {
            cpSide?.Dispose();
            client.Dispose();
            server?.Dispose();
            throw;
        }
        finally
        {
            listener.Stop();
        }
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

    public async Task<T> NextCommandAsync<T>(Func<T, bool>? wanted = null) where T : OsdpCommand
    {
        using var timeout = new CancellationTokenSource(Timeout);
        while (true)
        {
            if (await _commands.Reader.ReadAsync(timeout.Token) is T match && (wanted is null || wanted(match)))
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
        _cpSide.Dispose();
        _pdSide.Dispose();
        _stop.Dispose();
    }
}
