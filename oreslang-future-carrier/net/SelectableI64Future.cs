namespace Ores.Net.Runtime;

/// <summary>
/// Runtime-only selectable signed-i64 Future. Completion is immutable and
/// non-consuming: independent selects may observe the same terminal result.
/// Readiness callbacks are advisory and must revalidate their subscription
/// inside SelectArbiter.Registration.TryWin.
/// </summary>
public sealed class SelectableI64Future
{
    private const int MaxWaiterLimit = 4096;

    public enum FutureStatus
    {
        Pending,
        Value,
        Failed,
        Cancelled
    }

    public readonly struct Result
    {
        internal Result(FutureStatus status, long value, Exception? error)
        {
            Status = status;
            Value = value;
            Error = error;
        }

        public FutureStatus Status { get; }
        public long Value { get; }
        public Exception? Error { get; }
    }

    private sealed class Waiter
    {
        public ulong Id;
        public bool Active;
        public Action<Subscription>? Notify;
    }

    private readonly object _gate = new();
    private readonly Waiter[] _waiters;
    private ulong _nextWaiterId = 1;
    private FutureStatus _status;
    private long _value;
    private Exception? _error;
    private Exception? _notificationFailure;

    public SelectableI64Future(int maxWaiters = 64)
    {
        if (maxWaiters <= 0 || maxWaiters > MaxWaiterLimit)
            throw new ArgumentOutOfRangeException(nameof(maxWaiters),
                $"Future maxWaiters must be in 1..{MaxWaiterLimit}");

        _waiters = new Waiter[maxWaiters];
        for (var i = 0; i < maxWaiters; i++)
            _waiters[i] = new Waiter();
    }

    public FutureStatus Status
    {
        get { lock (_gate) return _status; }
    }

    public int PendingWaiters
    {
        get
        {
            lock (_gate)
            {
                var count = 0;
                foreach (var waiter in _waiters)
                    if (waiter.Active) count++;
                return count;
            }
        }
    }

    public Exception? NotificationFailure
    {
        get { lock (_gate) return _notificationFailure; }
    }

    public Subscription SubscribeReady(Action<Subscription> notify)
    {
        ArgumentNullException.ThrowIfNull(notify);

        Subscription subscription;
        var immediate = false;
        lock (_gate)
        {
            var index = -1;
            for (var i = 0; i < _waiters.Length; i++)
            {
                if (!_waiters[i].Active)
                {
                    index = i;
                    break;
                }
            }
            if (index < 0)
                throw new InvalidOperationException(
                    "Future select waiter capacity exhausted");
            if (_nextWaiterId == ulong.MaxValue)
                throw new OverflowException(
                    "Future select waiter identity exhausted");

            var id = _nextWaiterId++;
            subscription = new Subscription(this, index, id);
            var waiter = _waiters[index];
            waiter.Id = id;
            waiter.Active = true;
            waiter.Notify = notify;
            immediate = _status != FutureStatus.Pending;
        }

        if (immediate)
        {
            try
            {
                notify(subscription);
            }
            catch
            {
                _ = subscription.Close();
                throw;
            }
        }

        return subscription;
    }

    public bool TryCommit(Subscription subscription, out Result result)
    {
        lock (_gate)
        {
            if (!MatchesLocked(subscription) ||
                _status == FutureStatus.Pending)
            {
                result = new Result(FutureStatus.Pending, default, null);
                return false;
            }

            CloseLocked(_waiters[subscription.Index]);
            result = new Result(_status, _value, _error);
            return true;
        }
    }

    public bool Resolve(long value) =>
        Complete(FutureStatus.Value, value, null);

    public bool Fail(Exception error)
    {
        ArgumentNullException.ThrowIfNull(error);
        return Complete(FutureStatus.Failed, default, error);
    }

    public bool Cancel() =>
        Complete(FutureStatus.Cancelled, default, null);

    private bool Complete(
        FutureStatus status,
        long value,
        Exception? error)
    {
        var callbacks = new Action<Subscription>?[_waiters.Length];
        var subscriptions = new Subscription?[_waiters.Length];
        var count = 0;

        lock (_gate)
        {
            if (_status != FutureStatus.Pending) return false;

            for (var i = 0; i < _waiters.Length; i++)
            {
                var waiter = _waiters[i];
                if (!waiter.Active || waiter.Notify is null) continue;
                callbacks[count] = waiter.Notify;
                subscriptions[count] = new Subscription(this, i, waiter.Id);
                count++;
            }

            _status = status;
            _value = value;
            _error = error;
        }

        Exception? first = null;
        for (var i = 0; i < count; i++)
        {
            try
            {
                callbacks[i]!(subscriptions[i]!);
            }
            catch (Exception notificationError)
            {
                first ??= notificationError;
            }
        }

        if (first is not null)
        {
            lock (_gate)
                _notificationFailure ??= first;
        }

        return true;
    }

    private bool MatchesLocked(Subscription subscription)
    {
        if (!ReferenceEquals(subscription.Owner, this) ||
            (uint)subscription.Index >= (uint)_waiters.Length)
            return false;
        var waiter = _waiters[subscription.Index];
        return waiter.Active && waiter.Id == subscription.Id;
    }

    private bool Close(Subscription subscription)
    {
        lock (_gate)
        {
            if (!MatchesLocked(subscription)) return false;
            CloseLocked(_waiters[subscription.Index]);
            return true;
        }
    }

    private static void CloseLocked(Waiter waiter)
    {
        waiter.Active = false;
        waiter.Notify = null;
    }

    public sealed class Subscription
    {
        private readonly SelectableI64Future _owner;
        private readonly int _index;
        private readonly ulong _id;

        internal Subscription(
            SelectableI64Future owner,
            int index,
            ulong id)
        {
            _owner = owner;
            _index = index;
            _id = id;
        }

        internal SelectableI64Future Owner => _owner;
        internal int Index => _index;
        internal ulong Id => _id;

        public bool Active
        {
            get
            {
                lock (_owner._gate)
                    return _owner.MatchesLocked(this);
            }
        }

        public bool Close() => _owner.Close(this);
    }
}
