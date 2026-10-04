// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

namespace LibOSDP.Tests;

public sealed class DeviceTests
{
    [Fact]
    public async Task PlainPdComesOnline()
    {
        await using var bus = await Bus.StartAsync();
        Assert.True(bus.Cp.IsOnline(Bus.Address));
        Assert.False(bus.Cp.IsSecureChannelActive(Bus.Address));
        Assert.True(await bus.Pd.WaitOnlineAsync(Bus.Timeout, TestContext.Current.CancellationToken));
    }

    [Fact]
    public async Task SecureChannelComesUpWithASharedKey()
    {
        await using var bus = await Bus.StartAsync(BusOptions.Secure());
        Assert.True(bus.Cp.IsSecureChannelActive(Bus.Address));
        Assert.True(await bus.Pd.WaitSecureChannelAsync(Bus.Timeout, TestContext.Current.CancellationToken));
    }

    [Fact]
    public async Task InstallModePdTakesTheCpKey()
    {
        var options = new BusOptions
        {
            CpKey = BusOptions.Secure().CpKey,
            PdFlags = OsdpFlags.InstallMode,
        };
        await using var bus = await Bus.StartAsync(options);
        Assert.True(bus.Cp.IsSecureChannelActive(Bus.Address));
    }

    [Fact]
    public async Task UnknownAddressIsRejected()
    {
        await using var bus = await Bus.StartAsync();
        Assert.Throws<ArgumentException>(() => bus.Cp.IsOnline(42));
        await Assert.ThrowsAsync<ArgumentException>(
            () => bus.Cp.SubmitCommandAsync(42, new BuzzerCommand(0, BuzzerControlCode.Off, 0, 0, 0)));
    }

    [Fact]
    public void InvalidPdInfoIsRejected()
    {
        var (a, _) = LoopbackChannel.CreatePair();
        Assert.Throws<ArgumentOutOfRangeException>(() => new ControlPanel(a, [new PdInfo(127)]));
        Assert.Throws<ArgumentException>(() => new ControlPanel(a, [new PdInfo(1) { Scbk = new byte[8] }]));
        Assert.Throws<ArgumentException>(() => new ControlPanel(a, [new PdInfo(1), new PdInfo(1)]));
        Assert.Throws<ArgumentException>(() => new ControlPanel(a, []));
        Assert.Throws<ArgumentException>(() => new ControlPanel(a, [new PdInfo(1), null!]));
        Assert.Throws<ArgumentNullException>(() => new PeripheralDevice(a, null!, Bus.DefaultCapabilities));
    }

    [Fact]
    public async Task DisposeFailsPendingWaits()
    {
        var (cpSide, pdSide) = LoopbackChannel.CreatePair();
        var cp = new ControlPanel(cpSide, [new PdInfo(1)]);
        var pd = new PeripheralDevice(pdSide, new PdInfo(1), Bus.DefaultCapabilities);
        Task<bool> cpWait = cp.WaitOnlineAsync(1, Timeout.InfiniteTimeSpan, TestContext.Current.CancellationToken);
        Task<bool> pdWait = pd.WaitSecureChannelAsync(Timeout.InfiniteTimeSpan, TestContext.Current.CancellationToken);
        cp.Dispose();
        pd.Dispose();
        await Assert.ThrowsAsync<ObjectDisposedException>(
            () => cpWait.WaitAsync(Bus.Timeout, TestContext.Current.CancellationToken));
        await Assert.ThrowsAsync<ObjectDisposedException>(
            () => pdWait.WaitAsync(Bus.Timeout, TestContext.Current.CancellationToken));
        await Assert.ThrowsAsync<ObjectDisposedException>(
            () => cp.WaitOnlineAsync(1, Timeout.InfiniteTimeSpan, TestContext.Current.CancellationToken));
    }

    [Fact]
    public async Task HandlerExceptionSurfacesFromRunAsync()
    {
        await using var bus = await Bus.StartAsync();
        bus.LoopFaultExpected = true;
        bus.Reply = _ => throw new InvalidOperationException("boom");
        _ = bus.Cp.SubmitCommandAsync(Bus.Address, new BuzzerCommand(0, BuzzerControlCode.Off, 0, 0, 0));
        var fault = await Assert.ThrowsAsync<AggregateException>(
            () => bus.PdLoop.WaitAsync(Bus.Timeout, TestContext.Current.CancellationToken));
        Assert.Contains(fault.InnerExceptions, e => e is InvalidOperationException { Message: "boom" });
    }

    [Fact]
    public async Task DisposeFromAnEventHandlerIsRefused()
    {
        await using var bus = await Bus.StartAsync();
        bus.LoopFaultExpected = true;
        bus.Cp.EventReceived += (_, _) => bus.Cp.Dispose();
        _ = bus.Pd.SubmitEventAsync(new KeyPressEvent(0, [1]));
        var fault = await Assert.ThrowsAsync<AggregateException>(
            () => bus.CpLoop.WaitAsync(Bus.Timeout, TestContext.Current.CancellationToken));
        Assert.Contains(fault.InnerExceptions, e => e is InvalidOperationException);
    }

    [Fact]
    public async Task RefreshFromAnEventHandlerIsRefused()
    {
        await using var bus = await Bus.StartAsync();
        bus.LoopFaultExpected = true;
        bus.Cp.EventReceived += (_, _) => bus.Cp.Refresh();
        _ = bus.Pd.SubmitEventAsync(new KeyPressEvent(0, [1]));
        var fault = await Assert.ThrowsAsync<AggregateException>(
            () => bus.CpLoop.WaitAsync(Bus.Timeout, TestContext.Current.CancellationToken));
        Assert.Contains(fault.InnerExceptions, e => e is InvalidOperationException);
    }

    [Fact]
    public async Task DisposeFromACommandHandlerIsRefused()
    {
        await using var bus = await Bus.StartAsync();
        bus.LoopFaultExpected = true;
        bus.Reply = _ =>
        {
            bus.Pd.Dispose();
            return CommandReply.Ack;
        };
        _ = bus.Cp.SubmitCommandAsync(Bus.Address, new BuzzerCommand(0, BuzzerControlCode.Off, 0, 0, 0));
        var fault = await Assert.ThrowsAsync<AggregateException>(
            () => bus.PdLoop.WaitAsync(Bus.Timeout, TestContext.Current.CancellationToken));
        Assert.Contains(fault.InnerExceptions, e => e is InvalidOperationException);
    }

    [Fact]
    public async Task DisposedDeviceRefusesWork()
    {
        var bus = await Bus.StartAsync();
        await bus.DisposeAsync();
        Assert.Throws<ObjectDisposedException>(bus.Cp.Refresh);
        await Assert.ThrowsAsync<ObjectDisposedException>(
            () => bus.Cp.SubmitCommandAsync(Bus.Address, new BuzzerCommand(0, BuzzerControlCode.Off, 0, 0, 0)));
        bus.Cp.Dispose(); // idempotent
    }

    [Fact]
    public async Task DisposeAbortsQueuedCommands()
    {
        var bus = await Bus.StartAsync();
        var cmd = new OutputCommand(0, OutputControlCode.PermanentOff);
        var queued = new List<Task<CompletionStatus>>();
        for (int i = 0; i < 50; i++)
            queued.Add(bus.Cp.SubmitCommandAsync(Bus.Address, cmd));
        await bus.DisposeAsync();
        CompletionStatus[] statuses = await Task.WhenAll(queued).WaitAsync(Bus.Timeout, TestContext.Current.CancellationToken);
        Assert.Contains(CompletionStatus.Aborted, statuses);
    }

    [Fact]
    public async Task DisposeAbortsQueuedEvents()
    {
        var bus = await Bus.StartAsync();
        var queued = new List<Task<CompletionStatus>>();
        for (int i = 0; i < 50; i++)
            queued.Add(bus.Pd.SubmitEventAsync(new KeyPressEvent(0, [1])));
        await bus.DisposeAsync();
        CompletionStatus[] statuses = await Task.WhenAll(queued).WaitAsync(Bus.Timeout, TestContext.Current.CancellationToken);
        Assert.Contains(CompletionStatus.Aborted, statuses);
    }
}
