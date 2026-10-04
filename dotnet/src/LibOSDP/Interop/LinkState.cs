// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

namespace LibOSDP.Interop;

/// Online / secure-channel state per PD, fed by LibOSDP's notifications.
internal sealed class LinkState(int count)
{
    private sealed record Waiter(Func<bool> Satisfied, TaskCompletionSource<bool> Signal);

    private readonly object _gate = new();
    private readonly bool[] _online = new bool[count];
    private readonly bool[] _secure = new bool[count];
    private readonly List<Waiter> _waiters = [];
    private string? _closedBy;

    internal bool IsOnline(int pd)
    {
        lock (_gate)
            return _online[pd];
    }

    internal bool IsSecure(int pd)
    {
        lock (_gate)
            return _secure[pd];
    }

    internal void Apply(int pd, OsdpNotification notification)
    {
        lock (_gate)
        {
            switch (notification)
            {
            case PdStatusNotification status:
                _online[pd] = status.Online;
                if (!status.Online)
                    _secure[pd] = false; // no CP link means no secure channel, whichever is reported first
                break;
            case SecureChannelStatusNotification sc:
                _secure[pd] = sc.Active;
                break;
            default:
                return;
            }
            for (int i = _waiters.Count - 1; i >= 0; i--)
            {
                if (_waiters[i].Satisfied())
                {
                    _waiters[i].Signal.TrySetResult(true);
                    _waiters.RemoveAt(i);
                }
            }
        }
    }

    /// Fails every pending and future wait: the owning device is gone and no
    /// notification will ever arrive.
    internal void Close(string ownerName)
    {
        lock (_gate)
        {
            _closedBy = ownerName;
            foreach (Waiter waiter in _waiters)
                waiter.Signal.TrySetException(new ObjectDisposedException(ownerName));
            _waiters.Clear();
        }
    }

    internal async Task<bool> WaitAsync(Func<bool> satisfied, TimeSpan timeout, CancellationToken cancellationToken)
    {
        var waiter = new Waiter(satisfied, new(TaskCreationOptions.RunContinuationsAsynchronously));
        lock (_gate)
        {
            if (_closedBy is not null)
                throw new ObjectDisposedException(_closedBy);
            if (satisfied())
                return true;
            _waiters.Add(waiter);
        }
        try
        {
            return await waiter.Signal.Task.WaitAsync(timeout, cancellationToken).ConfigureAwait(false);
        }
        catch (TimeoutException)
        {
            return false;
        }
        finally
        {
            lock (_gate)
                _waiters.Remove(waiter);
        }
    }
}
