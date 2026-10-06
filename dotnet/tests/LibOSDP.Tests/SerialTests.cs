// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

using LibOSDP.Interop;
using Microsoft.Extensions.Logging;

namespace LibOSDP.Tests;

[CollectionDefinition(nameof(Serial), DisableParallelization = true)]
public sealed class Serial;

[Collection(nameof(Serial))]
public sealed class NativeHeapTests
{
    [Fact]
    public async Task DisposeWithQueuedWorkFreesEveryNativeAllocation()
    {
        long before = NativeHeap.Live;
        var bus = await Bus.StartAsync(BusOptions.Secure());
        for (int i = 0; i < 50; i++)
            _ = bus.Cp.SubmitCommandAsync(Bus.Address, new OutputCommand(0, OutputControlCode.PermanentOff));
        for (int i = 0; i < 20; i++)
            _ = bus.Pd.SubmitEventAsync(new KeyPressEvent(0, [1]));
        await bus.DisposeAsync();
        Assert.Equal(before, NativeHeap.Live);
    }

    [Fact]
    public void RejectedSetupFreesEveryNativeAllocation()
    {
        long before = NativeHeap.Live;
        var (a, _) = LoopbackChannel.CreatePair();
        Assert.Throws<ArgumentOutOfRangeException>(() => new ControlPanel(a, [new PdInfo(1), new PdInfo(127)]));
        Assert.Equal(before, NativeHeap.Live);
    }
}

[Collection(nameof(Serial))]
public sealed class LoggingTests
{
    private sealed class ListLogger : ILogger
    {
        public List<(LogLevel Level, string Message)> Entries { get; } = [];

        public IDisposable? BeginScope<TState>(TState state) where TState : notnull => null;

        public bool IsEnabled(LogLevel logLevel) => true;

        public void Log<TState>(LogLevel logLevel, EventId eventId, TState state, Exception? exception,
                                Func<TState, Exception?, string> formatter)
        {
            lock (Entries)
                Entries.Add((logLevel, formatter(state, exception)));
        }
    }

    private sealed class ThrowingLogger : ILogger
    {
        public IDisposable? BeginScope<TState>(TState state) where TState : notnull => null;
        public bool IsEnabled(LogLevel logLevel) => true;
        public void Log<TState>(LogLevel logLevel, EventId eventId, TState state, Exception? exception,
                                Func<TState, Exception?, string> formatter) => throw new InvalidOperationException();
    }

    [Fact]
    public async Task LibraryLogsReachTheLogger()
    {
        var logger = new ListLogger();
        Osdp.SetLogger(logger, LogLevel.Debug);
        try
        {
            await using var bus = await Bus.StartAsync();
        }
        finally
        {
            Osdp.SetLogger(null);
        }
        lock (logger.Entries)
            Assert.Contains(logger.Entries, e => e.Message.Contains("Setup complete"));
    }

    [Fact]
    public async Task ThrowingLoggerDoesNotStopTheBus()
    {
        Osdp.SetLogger(new ThrowingLogger(), LogLevel.Debug);
        try
        {
            await using var bus = await Bus.StartAsync();
            Assert.Equal(CompletionStatus.Ok, await bus.Cp
                .SubmitCommandAsync(Bus.Address, new BuzzerCommand(0, BuzzerControlCode.Off, 0, 0, 0))
                .WaitAsync(Bus.Timeout, TestContext.Current.CancellationToken));
        }
        finally
        {
            Osdp.SetLogger(null);
        }
    }
}
