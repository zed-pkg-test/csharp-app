using Ores.Net.Runtime;

internal static class SelectableI64ChannelSelfTest
{
    private static void Require(bool value, string reason)
    {
        if (!value) throw new Exception("Selectable channel regression: " + reason);
    }

    private static SelectableI64Channel.Subscription BindRead(
        SelectableI64Channel channel,
        SelectArbiter.Registration registration,
        Action<long> result,
        Action callback)
    {
        return channel.SubscribeReadable(subscription =>
        {
            callback();
            var notifications = channel.CreateNotificationBatch();
            SelectableI64Channel.ReadResult outcome = default;
            var won = registration.TryWin(() =>
                channel.TryCommitRead(subscription, out outcome, notifications));
            notifications.Dispatch();
            if (won &&
                outcome.Status == SelectableI64Channel.ReadStatus.Value)
                result(outcome.Value);
        });
    }

    private static SelectableI64Channel.Subscription BindWrite(
        SelectableI64Channel channel,
        SelectArbiter.Registration registration,
        long value,
        Action callback)
    {
        return channel.SubscribeWritable(subscription =>
        {
            callback();
            var notifications = channel.CreateNotificationBatch();
            SelectableI64Channel.WriteStatus outcome = default;
            _ = registration.TryWin(() =>
                channel.TryCommitWrite(
                    subscription, value, out outcome, notifications));
            notifications.Dispatch();
        });
    }

    private static void FirstReadyReadWinsWithoutConsumingLoser()
    {
        var firstChannel = new SelectableI64Channel(2);
        var secondChannel = new SelectableI64Channel(2);
        Require(firstChannel.TryWrite(11), "seed first");
        Require(secondChannel.TryWrite(22), "seed second");

        using var select = new SelectArbiter(2);
        var first = select.RegisterCase(0);
        var second = select.RegisterCase(1);
        long firstValue = 0;
        long secondValue = 0;
        var firstCallbacks = 0;
        var secondCallbacks = 0;

        var firstWait = BindRead(firstChannel, first,
            value => firstValue = value, () => firstCallbacks++);
        first.Arm(() => firstWait.Close());

        var secondWait = BindRead(secondChannel, second,
            value => secondValue = value, () => secondCallbacks++);
        second.Arm(() => secondWait.Close());

        Require(select.Winner == 0, "first ready read should win priority registration order");
        Require(firstValue == 11, "winning read value");
        Require(firstCallbacks == 1 && secondCallbacks == 1,
            "both immediate readiness callbacks observed");
        Require(firstChannel.Count == 0, "winner consumed exactly once");
        Require(secondChannel.Count == 1, "loser did not consume");
        Require(firstChannel.PendingReaders == 0 &&
                secondChannel.PendingReaders == 0,
            "loser readiness registration detached");

        Require(secondChannel.TryRead(out var untouched) && untouched == 22,
            "losing value remains available");
        Require(secondValue == 0, "losing binding never assigned");
    }

    private static void LosingWriteNeverPublishes()
    {
        var firstChannel = new SelectableI64Channel(2);
        var secondChannel = new SelectableI64Channel(2);
        using var select = new SelectArbiter(2);
        var first = select.RegisterCase(0);
        var second = select.RegisterCase(1);
        var firstCallbacks = 0;
        var secondCallbacks = 0;

        var firstWait = BindWrite(firstChannel, first, 31, () => firstCallbacks++);
        first.Arm(() => firstWait.Close());

        var secondWait = BindWrite(secondChannel, second, 32, () => secondCallbacks++);
        second.Arm(() => secondWait.Close());

        Require(select.Winner == 0, "first ready write wins");
        Require(firstCallbacks == 1 && secondCallbacks == 1,
            "both immediate write callbacks observed");
        Require(firstChannel.Count == 1, "winning write published");
        Require(secondChannel.Count == 0, "losing write never published");
        Require(firstChannel.PendingWriters == 0 &&
                secondChannel.PendingWriters == 0,
            "write waiters detached");

        Require(firstChannel.TryRead(out var committed) && committed == 31,
            "winning payload preserved");
        Require(!secondChannel.TryRead(out _), "losing payload absent");
    }

    private static void DelayedReadinessWakesPendingRead()
    {
        var firstChannel = new SelectableI64Channel(1);
        var secondChannel = new SelectableI64Channel(1);
        using var select = new SelectArbiter(2);
        var first = select.RegisterCase(0);
        var second = select.RegisterCase(1);
        long firstValue = 0;
        long secondValue = 0;
        var firstCallbacks = 0;
        var secondCallbacks = 0;

        var firstWait = BindRead(firstChannel, first,
            value => firstValue = value, () => firstCallbacks++);
        var secondWait = BindRead(secondChannel, second,
            value => secondValue = value, () => secondCallbacks++);
        first.Arm(() => firstWait.Close());
        second.Arm(() => secondWait.Close());

        Require(select.Winner is null, "empty channels leave select pending");
        Require(firstChannel.PendingReaders == 1 &&
                secondChannel.PendingReaders == 1,
            "both pending readers registered");

        Require(secondChannel.TryWrite(42), "producer write");
        Require(select.Winner == 1 && secondValue == 42,
            "second channel readiness wins and binds value");
        Require(firstValue == 0 && firstCallbacks == 0 && secondCallbacks == 1,
            "only ready callback ran");
        Require(firstChannel.PendingReaders == 0 &&
                secondChannel.PendingReaders == 0,
            "winner and loser registrations closed");

        Require(firstChannel.TryWrite(9), "later first-channel write");
        Require(firstCallbacks == 0,
            "detached loser callback never resurrected");
        Require(firstChannel.TryRead(out var untouched) && untouched == 9,
            "late loser channel value remains untouched");
    }

    private static void FreeingCapacityWakesPendingWrite()
    {
        var channel = new SelectableI64Channel(1);
        Require(channel.TryWrite(7), "fill channel");

        using var select = new SelectArbiter(1);
        var write = select.RegisterCase(0);
        var callbacks = 0;
        var wait = BindWrite(channel, write, 8, () => callbacks++);
        write.Arm(() => wait.Close());

        Require(select.Winner is null && channel.PendingWriters == 1,
            "full channel keeps write pending");

        Require(channel.TryRead(out var existing) && existing == 7,
            "read frees capacity");
        Require(select.Winner == 0 && callbacks == 1,
            "pending write wakes and wins");
        Require(channel.PendingWriters == 0,
            "winning write registration closes");
        Require(channel.TryRead(out var committed) && committed == 8,
            "winning write committed after capacity release");
    }

    private static void CancellationDetachesWaiter()
    {
        var channel = new SelectableI64Channel(1);
        using var select = new SelectArbiter(1);
        var read = select.RegisterCase(0);
        long value = 0;
        var callbacks = 0;

        var wait = BindRead(channel, read,
            v => value = v, () => callbacks++);
        read.Arm(() => wait.Close());
        Require(channel.PendingReaders == 1, "reader registered");

        Require(select.Cancel(), "select cancellation");
        Require(channel.PendingReaders == 0, "cancel detached channel waiter");
        Require(channel.TryWrite(55), "post-cancel producer");
        Require(callbacks == 0 && value == 0,
            "cancelled waiter never receives later readiness");
    }

    private static void StaleNotificationCannotConsumeForLoser()
    {
        var channel = new SelectableI64Channel(1);
        using var select = new SelectArbiter(2);
        var read = select.RegisterCase(0);
        var write = select.RegisterCase(1);
        long readValue = 0;
        var readCallbacks = 0;
        var writeCallbacks = 0;

        var readWait = BindRead(channel, read,
            value => readValue = value, () => readCallbacks++);
        read.Arm(() => readWait.Close());

        // The winning write captures the pending reader callback before it
        // mutates the buffer. Select loser cleanup then closes that reader
        // before the captured notification dispatches.
        var writeWait = BindWrite(channel, write, 99, () => writeCallbacks++);
        write.Arm(() => writeWait.Close());

        Require(select.Winner == 1, "write wins");
        Require(writeCallbacks == 1 && readCallbacks == 1,
            "stale captured reader callback may execute once");
        Require(readValue == 0, "stale loser callback cannot bind");
        Require(channel.PendingReaders == 0 &&
                channel.PendingWriters == 0,
            "no stale subscriptions remain");
        Require(channel.TryRead(out var value) && value == 99,
            "stale loser did not consume winning write value");
    }

    private static void ImmediateCallbackFailureDoesNotLeakWaiter()
    {
        var channel = new SelectableI64Channel(1, 2);
        Require(channel.TryWrite(1), "seed readable channel");

        var observed = false;
        try
        {
            _ = channel.SubscribeReadable(_ =>
                throw new InvalidOperationException("callback failed"));
        }
        catch (InvalidOperationException)
        {
            observed = true;
        }

        Require(observed, "immediate callback failure surfaced");
        Require(channel.PendingReaders == 0,
            "failed immediate subscriber removed");
        Require(channel.Count == 1, "callback failure did not mutate buffer");
    }

    private static void AsyncNotificationFailureIsRecorded()
    {
        var channel = new SelectableI64Channel(1, 2);
        var wait = channel.SubscribeReadable(_ =>
            throw new InvalidOperationException("async callback failed"));
        Require(channel.PendingReaders == 1, "reader registered");

        Require(channel.TryWrite(5), "write commits despite callback failure");
        Require(channel.Count == 1, "committed write remains visible");
        Require(channel.NotificationFailure is InvalidOperationException,
            "notification failure retained for supervision");
        Require(wait.Close(), "failed callback registration remains revocable");
        Require(channel.PendingReaders == 0, "failed waiter detached");
    }

    private static void CloseWakesPendingReadWithEof()
    {
        var channel = new SelectableI64Channel(1);
        using var select = new SelectArbiter(1);
        var read = select.RegisterCase(0);
        SelectableI64Channel.ReadResult outcome = default;
        var callbacks = 0;

        var wait = channel.SubscribeReadable(subscription =>
        {
            callbacks++;
            var notifications = channel.CreateNotificationBatch();
            _ = read.TryWin(() =>
                channel.TryCommitRead(
                    subscription, out outcome, notifications));
            notifications.Dispatch();
        });
        read.Arm(() => wait.Close());

        Require(select.Winner is null, "empty channel keeps read pending");
        Require(channel.Close(), "first close transitions channel");
        Require(!channel.Close(), "close is idempotent");
        Require(channel.Closed, "closed state visible");
        Require(select.Winner == 0 && callbacks == 1,
            "close wakes pending selected read");
        Require(outcome.Status == SelectableI64Channel.ReadStatus.Eof,
            "selected read commits EOF");
        Require(channel.PendingReaders == 0, "EOF reader detached");
        Require(!channel.TryWrite(1), "post-close writes rejected");
        Require(channel.TryReadResult().Status ==
                SelectableI64Channel.ReadStatus.Eof,
            "direct read distinguishes EOF");
    }

    private static void CloseDrainsBufferedValueBeforeEof()
    {
        var channel = new SelectableI64Channel(1);
        Require(channel.TryWrite(77), "seed close-drain buffer");
        Require(channel.Close(), "close buffered channel");

        var value = channel.TryReadResult();
        Require(value.Status == SelectableI64Channel.ReadStatus.Value &&
                value.Value == 77,
            "buffered value drains after close");
        Require(channel.TryReadResult().Status ==
                SelectableI64Channel.ReadStatus.Eof,
            "EOF follows final buffered value");
    }

    private static void CloseWakesPendingWriteAsClosed()
    {
        var channel = new SelectableI64Channel(1);
        Require(channel.TryWrite(7), "fill close-write channel");
        using var select = new SelectArbiter(1);
        var write = select.RegisterCase(0);
        SelectableI64Channel.WriteStatus outcome = default;
        var callbacks = 0;

        var wait = channel.SubscribeWritable(subscription =>
        {
            callbacks++;
            var notifications = channel.CreateNotificationBatch();
            _ = write.TryWin(() =>
                channel.TryCommitWrite(
                    subscription, 8, out outcome, notifications));
            notifications.Dispatch();
        });
        write.Arm(() => wait.Close());

        Require(select.Winner is null, "full channel keeps write pending");
        Require(channel.Close(), "close wakes pending write");
        Require(select.Winner == 0 && callbacks == 1,
            "closed write arm becomes terminal winner");
        Require(outcome == SelectableI64Channel.WriteStatus.Closed,
            "selected write reports closed outcome");
        Require(channel.PendingWriters == 0, "closed writer detached");
        Require(channel.TryRead(out var existing) && existing == 7,
            "closed write did not publish payload");
        Require(channel.TryReadResult().Status ==
                SelectableI64Channel.ReadStatus.Eof,
            "buffer drains to EOF");
    }

    private static void ForeignSubscriptionCannotCommit()
    {
        var first = new SelectableI64Channel(1);
        var second = new SelectableI64Channel(1);
        var foreign = first.SubscribeReadable(_ => { });

        var batch = second.CreateNotificationBatch();
        Require(!second.TryCommitRead(foreign, out _, batch),
            "foreign read subscription rejected");
        Require(!second.TryCommitWrite(foreign, 1, out _, batch),
            "foreign write subscription rejected");
        Require(foreign.Close(), "foreign source subscription closes");
    }

    public static void Run()
    {
        FirstReadyReadWinsWithoutConsumingLoser();
        LosingWriteNeverPublishes();
        DelayedReadinessWakesPendingRead();
        FreeingCapacityWakesPendingWrite();
        CancellationDetachesWaiter();
        StaleNotificationCannotConsumeForLoser();
        ImmediateCallbackFailureDoesNotLeakWaiter();
        AsyncNotificationFailureIsRecorded();
        CloseWakesPendingReadWithEof();
        CloseDrainsBufferedValueBeforeEof();
        CloseWakesPendingWriteAsClosed();
        ForeignSubscriptionCannotCommit();
    }
}
