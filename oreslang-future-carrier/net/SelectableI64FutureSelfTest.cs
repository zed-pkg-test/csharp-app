using Ores.Net.Runtime;

internal static class SelectableI64FutureSelfTest
{
    private static void Require(bool value, string reason)
    {
        if (!value) throw new Exception("Selectable Future regression: " + reason);
    }

    private static SelectableI64Future.Subscription Bind(
        SelectableI64Future future,
        SelectArbiter.Registration registration,
        Action<SelectableI64Future.Result> result,
        Action? callback = null)
    {
        return future.SubscribeReady(subscription =>
        {
            callback?.Invoke();
            SelectableI64Future.Result outcome = default;
            var won = registration.TryWin(() =>
                future.TryCommit(subscription, out outcome));
            if (won) result(outcome);
        });
    }

    private static void PendingResolveWinsWithValue()
    {
        var future = new SelectableI64Future();
        using var select = new SelectArbiter(1);
        var arm = select.RegisterCase(0);
        SelectableI64Future.Result result = default;
        var callbacks = 0;
        var wait = Bind(future, arm, r => result = r, () => callbacks++);
        arm.Arm(() => wait.Close());

        Require(select.Winner is null, "pending Future keeps select pending");
        Require(future.Resolve(42), "first resolution succeeds");
        Require(select.Winner == 0, "resolved Future wins");
        Require(callbacks == 1, "one readiness callback");
        Require(result.Status == SelectableI64Future.FutureStatus.Value &&
                result.Value == 42,
            "winning result carries value");
        Require(future.PendingWaiters == 0, "winning subscription closes");
    }

    private static void CompletionIsImmutableAndNonConsuming()
    {
        var future = new SelectableI64Future();
        Require(future.Resolve(7), "initial completion");
        Require(!future.Resolve(8), "second resolution rejected");
        Require(!future.Cancel(), "terminal Future cannot cancel");

        for (var i = 0; i < 2; i++)
        {
            using var select = new SelectArbiter(1);
            var arm = select.RegisterCase(0);
            SelectableI64Future.Result result = default;
            var wait = Bind(future, arm, r => result = r);
            arm.Arm(() => wait.Close());
            Require(select.Winner == 0, "terminal Future immediately ready");
            Require(result.Status == SelectableI64Future.FutureStatus.Value &&
                    result.Value == 7,
                "independent observer sees same immutable value");
        }
    }

    private static void StaleSameFutureNotificationCannotWinLoser()
    {
        var future = new SelectableI64Future();
        using var select = new SelectArbiter(2);
        var first = select.RegisterCase(0);
        var second = select.RegisterCase(1);
        var firstCallbacks = 0;
        var secondCallbacks = 0;
        var sideEffects = 0;

        var firstWait = Bind(future, first,
            _ => sideEffects++, () => firstCallbacks++);
        var secondWait = Bind(future, second,
            _ => sideEffects += 100, () => secondCallbacks++);
        first.Arm(() => firstWait.Close());
        second.Arm(() => secondWait.Close());

        Require(future.Resolve(9), "completion");
        Require(select.Winner == 0, "first registration wins");
        Require(firstCallbacks == 1 && secondCallbacks == 1,
            "captured stale loser callback may execute");
        Require(sideEffects == 1,
            "stale losing callback cannot run arm side effect");
        Require(future.PendingWaiters == 0, "winner and loser detached");
    }

    private static void SelectCancellationRevokesSubscription()
    {
        var future = new SelectableI64Future();
        using var select = new SelectArbiter(1);
        var arm = select.RegisterCase(0);
        var callbacks = 0;
        var wait = Bind(future, arm, _ => { }, () => callbacks++);
        arm.Arm(() => wait.Close());

        Require(future.PendingWaiters == 1, "waiter registered");
        Require(select.Cancel(), "select cancellation");
        Require(future.PendingWaiters == 0, "waiter revoked");
        Require(future.Resolve(5), "Future may complete after select cancel");
        Require(callbacks == 0, "revoked callback never fires");
    }

    private static void CancellationAndFailureAreTerminalOutcomes()
    {
        var cancelled = new SelectableI64Future();
        using (var select = new SelectArbiter(1))
        {
            var arm = select.RegisterCase(0);
            SelectableI64Future.Result result = default;
            var wait = Bind(cancelled, arm, r => result = r);
            arm.Arm(() => wait.Close());
            Require(cancelled.Cancel(), "Future cancellation");
            Require(select.Winner == 0 &&
                    result.Status == SelectableI64Future.FutureStatus.Cancelled,
                "cancelled Future is selectable");
        }

        var failed = new SelectableI64Future();
        using (var select = new SelectArbiter(1))
        {
            var arm = select.RegisterCase(0);
            SelectableI64Future.Result result = default;
            var wait = Bind(failed, arm, r => result = r);
            arm.Arm(() => wait.Close());
            var failure = new InvalidOperationException("boom");
            Require(failed.Fail(failure), "Future failure");
            Require(select.Winner == 0 &&
                    result.Status == SelectableI64Future.FutureStatus.Failed &&
                    ReferenceEquals(result.Error, failure),
                "failed Future preserves failure");
        }
    }

    private static void NotificationFailureIsRecorded()
    {
        var future = new SelectableI64Future(2);
        var wait = future.SubscribeReady(_ =>
            throw new InvalidOperationException("notify failed"));
        Require(future.Resolve(1), "completion survives callback failure");
        Require(future.NotificationFailure is InvalidOperationException,
            "notification failure retained");
        Require(wait.Close(), "failed callback waiter remains revocable");
        Require(future.PendingWaiters == 0, "failed waiter detached");
    }

    private static void ForeignSubscriptionFailsClosed()
    {
        var first = new SelectableI64Future();
        var second = new SelectableI64Future();
        var foreign = first.SubscribeReady(_ => { });
        Require(!second.TryCommit(foreign, out _),
            "foreign subscription cannot commit");
        Require(foreign.Close(), "source subscription remains revocable");
    }

    public static void Run()
    {
        PendingResolveWinsWithValue();
        CompletionIsImmutableAndNonConsuming();
        StaleSameFutureNotificationCannotWinLoser();
        SelectCancellationRevokesSubscription();
        CancellationAndFailureAreTerminalOutcomes();
        NotificationFailureIsRecorded();
        ForeignSubscriptionFailsClosed();
    }
}
