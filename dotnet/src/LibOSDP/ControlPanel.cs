// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

using System.Runtime.InteropServices;
using LibOSDP.Interop;

namespace LibOSDP;

/// <summary>
/// An OSDP control panel driving one or more peripheral devices over a shared
/// bus. Call <see cref="RunAsync"/> (or <see cref="Refresh"/> at least every
/// 50 ms) to service the bus, and always <see cref="Dispose"/> it: LibOSDP
/// holds a handle to this object, so it is never garbage collected otherwise.
/// </summary>
public sealed unsafe class ControlPanel : IDisposable
{
    private readonly object _gate = new();
    private readonly int[] _addresses;
    private readonly bool[] _forwardNotifications;
    private readonly LinkState _link;
    private readonly HandlerErrors _errors = new();
    private readonly PendingSubmissions _pending = new();
    private readonly List<GCHandle> _fileOps = [];
    private GCHandle _self;
    private GCHandle _channel;
    private nint _ctx;
    private int _disposed;

    /// <summary>
    /// A PD reported an event. Raised on the refresh thread while the bus is
    /// locked: keep handlers short, and never block on a task that needs the
    /// bus to make progress. Handlers must not call <see cref="Refresh"/> or
    /// <see cref="Dispose"/>; both throw <see cref="InvalidOperationException"/>
    /// there, which then surfaces from <see cref="Refresh"/>.
    /// </summary>
    public event EventHandler<OsdpEventArgs>? EventReceived;

    /// <summary>Set up a control panel.</summary>
    /// <param name="bus">The channel all PDs share.</param>
    /// <param name="pds">The PDs on the bus; addresses must be unique.</param>
    public ControlPanel(OsdpChannel bus, IReadOnlyList<PdInfo> pds)
    {
        ArgumentNullException.ThrowIfNull(bus);
        ArgumentNullException.ThrowIfNull(pds);
        if (pds.Count == 0)
            throw new ArgumentException("A control panel needs at least one PD.", nameof(pds));
        if (pds.Any(p => p is null))
            throw new ArgumentException("The PD list must not contain null.", nameof(pds));
        _addresses = pds.Select(p => p.Address).ToArray();
        if (_addresses.Distinct().Count() != _addresses.Length)
            throw new ArgumentException("PD addresses must be unique.", nameof(pds));
        Osdp.EnsureInitialized();

        _forwardNotifications = pds.Select(p => p.Flags.HasFlag(OsdpFlags.EnableNotification)).ToArray();
        _link = new LinkState(pds.Count);
        _self = GCHandle.Alloc(this);
        _channel = GCHandle.Alloc(new ChannelHost(bus, _errors));
        NativeChannel channel = ChannelBridge.Describe(_channel);
        try
        {
            // Notifications feed IsOnline and friends, so LibOSDP always sends
            // them; the caller's flag decides whether EventReceived sees them.
            using var infos = new NativePdInfoList(pds, OsdpFlags.EnableNotification, capabilities: null);
            _ctx = Native.CpSetup(&channel, pds.Count, infos.Pointer);
            if (_ctx == 0)
                throw new OsdpException("osdp_cp_setup failed; check the PD list.");
        }
        catch
        {
            _self.Free();
            _channel.Free();
            throw;
        }
        nint self = GCHandle.ToIntPtr(_self);
        Native.CpSetEventCallback(_ctx, &OnEvent, self);
        Native.CpSetCommandCompletionCallback(_ctx, &OnCommandCompleted, self);
    }

    /// <summary>Service the bus once. Rethrows anything a handler or the channel threw.</summary>
    /// <exception cref="InvalidOperationException">Called from inside <see cref="EventReceived"/>.</exception>
    public void Refresh()
    {
        ThrowIfInsideCallback(nameof(Refresh));
        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_ctx == 0, this);
            Native.CpRefresh(_ctx);
        }
        _errors.ThrowIfAny();
    }

    /// <summary>
    /// Service the bus every <see cref="Osdp.RefreshInterval"/> until cancelled
    /// (throws <see cref="OperationCanceledException"/>) or disposed (returns).
    /// </summary>
    /// <param name="cancellationToken">Stops the loop.</param>
    /// <returns>A task that faults if a handler or the channel threw.</returns>
    public Task RunAsync(CancellationToken cancellationToken = default) =>
        RefreshLoop.RunAsync(Refresh, () => Volatile.Read(ref _disposed) != 0, cancellationToken);

    /// <summary>
    /// Queue a command. The returned task completes when LibOSDP is done with
    /// it: answered, failed, flushed, or aborted by <see cref="Dispose"/>.
    /// </summary>
    /// <param name="address">Address of the target PD.</param>
    /// <param name="command">The command.</param>
    /// <returns>How the command settled.</returns>
    /// <exception cref="ArgumentException">Unknown address or invalid command.</exception>
    /// <exception cref="OsdpException">The PD cannot take it now (offline, or the command needs a secure channel).</exception>
    public Task<CompletionStatus> SubmitCommandAsync(int address, OsdpCommand command)
    {
        ArgumentNullException.ThrowIfNull(command);
        int pd = IndexOf(address);
        var cmd = (NativeCmd*)NativeHeap.AllocZeroed((nuint)sizeof(NativeCmd));
        try
        {
            CommandMarshal.ToNative(command, cmd);
        }
        catch
        {
            NativeHeap.Free(cmd);
            throw;
        }
        Task<CompletionStatus> completion = _pending.Track(cmd, sizeof(NativeCmd));
        int rc;
        lock (_gate)
            rc = _ctx == 0 ? int.MinValue : Native.CpSubmitCommand(_ctx, pd, cmd);
        if (rc == 0)
            return completion;
        _pending.Discard(cmd);
        ObjectDisposedException.ThrowIf(rc == int.MinValue, this);
        throw new OsdpException($"PD {address} cannot take {command.GetType().Name} now.");
    }

    /// <summary>Whether the PD is online.</summary>
    /// <param name="address">PD address.</param>
    /// <returns>True when online.</returns>
    public bool IsOnline(int address) => _link.IsOnline(IndexOf(address));

    /// <summary>Whether the PD's secure channel is active.</summary>
    /// <param name="address">PD address.</param>
    /// <returns>True when active.</returns>
    public bool IsSecureChannelActive(int address) => _link.IsSecure(IndexOf(address));

    /// <summary>Wait until the PD is online.</summary>
    /// <param name="address">PD address.</param>
    /// <param name="timeout">How long to wait.</param>
    /// <param name="cancellationToken">Stops waiting.</param>
    /// <returns>False on timeout.</returns>
    public Task<bool> WaitOnlineAsync(int address, TimeSpan timeout, CancellationToken cancellationToken = default)
    {
        int pd = IndexOf(address);
        return _link.WaitAsync(() => _link.IsOnline(pd), timeout, cancellationToken);
    }

    /// <summary>Wait until the PD's secure channel is active.</summary>
    /// <param name="address">PD address.</param>
    /// <param name="timeout">How long to wait.</param>
    /// <param name="cancellationToken">Stops waiting.</param>
    /// <returns>False on timeout.</returns>
    public Task<bool> WaitSecureChannelAsync(int address, TimeSpan timeout, CancellationToken cancellationToken = default)
    {
        int pd = IndexOf(address);
        return _link.WaitAsync(() => _link.IsSecure(pd), timeout, cancellationToken);
    }

    /// <summary>Tear down the bus. Queued commands complete as <see cref="CompletionStatus.Aborted"/>.</summary>
    /// <exception cref="InvalidOperationException">Called from inside <see cref="EventReceived"/>.</exception>
    public void Dispose()
    {
        ThrowIfInsideCallback(nameof(Dispose));
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
            return;
        lock (_gate)
        {
            Native.CpTeardown(_ctx); // runs the Aborted completions
            _ctx = 0;
        }
        _pending.AbortRemaining();
        _link.Close(nameof(ControlPanel));
        _self.Free();
        _channel.Free();
        foreach (GCHandle handle in _fileOps)
            handle.Free();
    }

    internal int IndexOf(int address)
    {
        int index = Array.IndexOf(_addresses, address);
        if (index < 0)
            throw new ArgumentException($"No PD with address {address} on this bus.", nameof(address));
        return index;
    }

    // Only the refresh thread holds _gate while user code runs, so holding it
    // here means a handler is calling back in. Refreshing or tearing down the
    // context from inside its own refresh crashes LibOSDP.
    private void ThrowIfInsideCallback(string member)
    {
        if (Monitor.IsEntered(_gate))
            throw new InvalidOperationException(
                $"{member} cannot be called from inside a LibOSDP callback (EventReceived, CommandHandler, or a channel operation).");
    }

    private static ControlPanel? From(nint arg)
    {
        try
        {
            return GCHandle.FromIntPtr(arg).Target as ControlPanel;
        }
        catch (InvalidOperationException)
        {
            return null;
        }
    }

    [UnmanagedCallersOnly]
    private static int OnEvent(nint arg, int pd, NativeEvent* ev)
    {
        ControlPanel? self = From(arg);
        if (self is null)
            return -1;
        try
        {
            OsdpEvent? decoded = EventMarshal.FromNative(ev);
            if (decoded is null)
                return 0;
            if (decoded is NotificationEvent note)
            {
                self._link.Apply(pd, note.Notification);
                if (!self._forwardNotifications[pd])
                    return 0;
            }
            self.EventReceived?.Invoke(self, new OsdpEventArgs(self._addresses[pd], decoded));
            return 0;
        }
        catch (Exception e)
        {
            self._errors.Record(e);
            return -1;
        }
    }

    [UnmanagedCallersOnly]
    private static void OnCommandCompleted(nint arg, int pd, NativeCmd* cmd, int status) =>
        From(arg)?._pending.Complete(cmd, (CompletionStatus)status);
}
