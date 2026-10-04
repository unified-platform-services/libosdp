// Copyright (c) 2026 Siddharth Chandrasekaran <sidcha.dev@gmail.com>
//
// SPDX-License-Identifier: Apache-2.0

namespace LibOSDP.Interop;

internal static class RefreshLoop
{
    internal static async Task RunAsync(Action refresh, Func<bool> disposed, CancellationToken cancellationToken)
    {
        using var timer = new PeriodicTimer(Osdp.RefreshInterval);
        try
        {
            do
            {
                refresh();
            }
            while (await timer.WaitForNextTickAsync(cancellationToken).ConfigureAwait(false));
        }
        catch (ObjectDisposedException) when (disposed())
        {
            // Disposing the device is a normal way to stop the loop.
        }
    }
}
