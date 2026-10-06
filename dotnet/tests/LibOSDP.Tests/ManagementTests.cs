// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

namespace LibOSDP.Tests;

public sealed class ManagementTests
{
    // osdp_cp_disable_pd/enable_pd only post a request; the state machine
    // applies it on the next refresh (osdp_cp.c).
    private static async Task WaitUntilAsync(Func<bool> condition)
    {
        using var timeout = new CancellationTokenSource(Bus.Timeout);
        while (!condition())
            await Task.Delay(10, timeout.Token);
    }

    [Fact]
    public async Task CpLearnsThePdIdentityAndCapabilities()
    {
        var id = new PdId(Version: 1, Model: 2, VendorCode: 0xCAFEBA, SerialNumber: 0xDEADBEEF, FirmwareVersion: 0x010203);
        await using var bus = await Bus.StartAsync(new BusOptions { PdId = id });
        Assert.Equal(id, bus.Cp.GetPdId(Bus.Address));
        Assert.Equal(new PdCapability(Capability.OutputControl, 1, 8),
                     bus.Cp.GetCapability(Bus.Address, Capability.OutputControl));
    }

    [Fact]
    public async Task UnadvertisedCapabilityIsNull()
    {
        await using var bus = await Bus.StartAsync();
        Assert.Null(bus.Cp.GetCapability(Bus.Address, Capability.SmartCard));
    }

    [Fact]
    public async Task MetricsCountTrafficAndReset()
    {
        await using var bus = await Bus.StartAsync();
        Metrics first = bus.Cp.GetMetrics(Bus.Address);
        Assert.True(first.PacketsSent > 0);
        Assert.True(bus.Pd.GetMetrics().PacketsReceived > 0);
    }

    [Fact]
    public async Task DisabledPdGoesOfflineAndComesBack()
    {
        await using var bus = await Bus.StartAsync();
        bus.Cp.DisablePd(Bus.Address);
        await WaitUntilAsync(() => !bus.Cp.IsPdEnabled(Bus.Address));
        bus.Cp.EnablePd(Bus.Address);
        await WaitUntilAsync(() => bus.Cp.IsPdEnabled(Bus.Address));
        Assert.True(await bus.Cp.WaitOnlineAsync(Bus.Address, Bus.Timeout, TestContext.Current.CancellationToken));
    }

    [Fact]
    public async Task FlagsCanBeSetAndCleared()
    {
        await using var bus = await Bus.StartAsync();
        Assert.True(bus.Cp.SetFlag(Bus.Address, OsdpFlags.IgnoreUnsolicited));
        Assert.True(bus.Cp.ClearFlag(Bus.Address, OsdpFlags.IgnoreUnsolicited));
    }

    [Fact]
    public async Task CancelWithNothingRunningReturnsFalse()
    {
        await using var bus = await Bus.StartAsync();
        Assert.False(bus.Cp.Cancel(Bus.Address, MpMsgType.FileTransfer));
        Assert.Null(bus.Cp.GetFileTransferStatus(Bus.Address));
    }

    [Fact]
    public async Task FlushedCommandsCompleteAsFlushed()
    {
        await using var bus = await Bus.StartAsync();
        var queued = new List<Task<CompletionStatus>>();
        for (int i = 0; i < 50; i++)
            queued.Add(bus.Cp.SubmitCommandAsync(Bus.Address, new OutputCommand(0, OutputControlCode.PermanentOff)));
        Assert.True(bus.Cp.FlushCommands(Bus.Address) > 0);
        CompletionStatus[] statuses = await Task.WhenAll(queued).WaitAsync(Bus.Timeout, TestContext.Current.CancellationToken);
        Assert.Contains(CompletionStatus.Flushed, statuses);
    }

    [Fact]
    public async Task FlushedEventsCompleteAsFlushed()
    {
        await using var bus = await Bus.StartAsync();
        var queued = new List<Task<CompletionStatus>>();
        for (int i = 0; i < 50; i++)
            queued.Add(bus.Pd.SubmitEventAsync(new KeyPressEvent(0, [1])));
        Assert.True(bus.Pd.FlushEvents() > 0);
        CompletionStatus[] statuses = await Task.WhenAll(queued).WaitAsync(Bus.Timeout, TestContext.Current.CancellationToken);
        Assert.Contains(CompletionStatus.Flushed, statuses);
    }

    private sealed class UnpluggedChannel : OsdpChannel
    {
        private int _reads;

        public int Reads => Volatile.Read(ref _reads);

        public override int Read(Span<byte> buffer)
        {
            Interlocked.Increment(ref _reads);
            throw new IOException("unplugged");
        }

        public override int Write(ReadOnlySpan<byte> buffer) => buffer.Length;
    }

    [Fact]
    public async Task ChannelErrorsSurfaceOnlyFromRefresh()
    {
        var channel = new UnpluggedChannel();
        using var cp = new ControlPanel(channel, [new PdInfo(Bus.Address)]);
        using var stop = new CancellationTokenSource(TestTime.Scaled(TimeSpan.FromMilliseconds(500)));
        Task management = Task.Run(() =>
        {
            while (!stop.IsCancellationRequested)
            {
                cp.IsPdEnabled(Bus.Address);
                cp.FlushCommands(Bus.Address);
            }
        }, TestContext.Current.CancellationToken);
        int surfaced = 0;
        while (!stop.IsCancellationRequested)
        {
            try
            {
                cp.Refresh();
            }
            catch (AggregateException e)
            {
                Assert.All(e.InnerExceptions, inner => Assert.Equal("unplugged", inner.Message));
                surfaced += e.InnerExceptions.Count;
            }
        }
        await management;
        Assert.True(surfaced > 0);
        Assert.Equal(channel.Reads, surfaced);

        var fault = await Assert.ThrowsAsync<AggregateException>(
            () => cp.RunAsync(TestContext.Current.CancellationToken).WaitAsync(Bus.Timeout, TestContext.Current.CancellationToken));
        Assert.Contains(fault.InnerExceptions, e => e is IOException { Message: "unplugged" });
    }
}
