// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

namespace LibOSDP.Tests;

public sealed class FileTransferTests
{
    internal sealed class SendFile(byte[] content) : IFileOperations
    {
        public bool Open(int fileId, ref uint size)
        {
            size = (uint)content.Length;
            return true;
        }

        public int Read(Span<byte> buffer, uint offset)
        {
            int n = Math.Min(buffer.Length, content.Length - (int)offset);
            content.AsSpan((int)offset, n).CopyTo(buffer);
            return n;
        }

        public int Write(ReadOnlySpan<byte> buffer, uint offset) => -1;

        public void Close() { }
    }

    internal sealed class ReceiveFile : IFileOperations
    {
        private readonly MemoryStream _data = new();

        public byte[] Content
        {
            get { lock (_data) return _data.ToArray(); }
        }

        public bool Open(int fileId, ref uint size) => true;

        public int Read(Span<byte> buffer, uint offset) => -1;

        public int Write(ReadOnlySpan<byte> buffer, uint offset)
        {
            lock (_data)
            {
                _data.Position = offset;
                _data.Write(buffer);
            }
            return buffer.Length;
        }

        public void Close() { }
    }

    private sealed class OverReportingSender : IFileOperations
    {
        public bool Open(int fileId, ref uint size)
        {
            size = 4000;
            return true;
        }

        public int Read(Span<byte> buffer, uint offset) => buffer.Length + 1;

        public int Write(ReadOnlySpan<byte> buffer, uint offset) => -1;

        public void Close() { }
    }

    [Fact]
    public async Task ReadReportingMoreThanTheBufferFailsTheTransfer()
    {
        var receiver = new ReceiveFile();
        await using var bus = await Bus.StartAsync(BusOptions.Secure());
        bus.LoopFaultExpected = true;
        bus.Cp.RegisterFileOperations(Bus.Address, new OverReportingSender());
        bus.Pd.RegisterFileOperations(receiver);

        Task<CompletionStatus> command = bus.Cp.SubmitCommandAsync(Bus.Address, new FileTransferCommand(FileId: 1));
        var fault = await Assert.ThrowsAsync<AggregateException>(() => bus.CpLoop.WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken));
        Assert.Contains(fault.InnerExceptions, e => e is InvalidOperationException);
        Assert.Empty(receiver.Content);
        if (command.IsCompleted)
            Assert.NotEqual(CompletionStatus.Ok, await command);
    }

    [Fact]
    public async Task FileArrivesIntactAndTheCommandCompletesAtTheEnd()
    {
        byte[] content = Enumerable.Range(0, 4000).Select(i => (byte)(i * 7)).ToArray();
        var receiver = new ReceiveFile();
        await using var bus = await Bus.StartAsync(BusOptions.Secure());
        bus.Cp.RegisterFileOperations(Bus.Address, new SendFile(content));
        bus.Pd.RegisterFileOperations(receiver);

        CompletionStatus status = await bus.Cp
            .SubmitCommandAsync(Bus.Address, new FileTransferCommand(FileId: 1))
            .WaitAsync(TimeSpan.FromSeconds(30), TestContext.Current.CancellationToken);

        Assert.Equal(CompletionStatus.Ok, status);
        Assert.Equal(content, receiver.Content);
    }
}
