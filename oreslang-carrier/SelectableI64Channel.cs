namespace Ores.Net.Runtime;

/// <summary>
/// Runtime-only bounded i64 channel with revocable readiness subscriptions.
/// This is a substrate for future compiler-lowered static select; it is not
/// source Channel&lt;T&gt; support.
/// </summary>
public sealed class SelectableI64Channel
{
    private const int MaxCapacity = 1_048_576;
    private const int MaxWaiterLimit = 4096;

    public enum ReadStatus
    {
        WouldBlock,
        Value,
        Eof
    }

    public readonly struct ReadResult
    {
        internal ReadResult(ReadStatus status, long value = default)
        {
            Status = status;
            Value = value;
        }

        public ReadStatus Status { get; }
        public long Value { get; }
    }

    public enum WriteStatus
    {
        WouldBlock,
        Written,
        Closed
    }

    internal enum WaitKind
    {
        Read,
        Write
    }

    private sealed class Waiter
    {
        public ulong Id;
        public bool Active;
        public Action<Subscription>? Notify;
    }

    private readonly object _gate = new();
    private readonly long[] _values;
    private readonly Waiter[] _readers;
    private readonly Waiter[] _writers;
    private int _head;
    private int _tail;
    private int _count;
    private ulong _nextWaiterId = 1;
    private Exception? _notificationFailure;
    private bool _closed;

    public SelectableI64Channel(int capacity, int maxWaiters = 64)
    {
        if (capacity <= 0 || capacity > MaxCapacity)
            throw new ArgumentOutOfRangeException(nameof(capacity),
                $"Channel capacity must be in 1..{MaxCapacity}");
        if (maxWaiters <= 0 || maxWaiters > MaxWaiterLimit)
            throw new ArgumentOutOfRangeException(nameof(maxWaiters),
                $"Channel maxWaiters must be in 1..{MaxWaiterLimit}");

        _values = new long[capacity];
        _readers = CreateWaiters(maxWaiters);
        _writers = CreateWaiters(maxWaiters);
    }

    public int Capacity => _values.Length;

    public int Count
    {
        get { lock (_gate) return _count; }
    }

    public bool Closed
    {
        get { lock (_gate) return _closed; }
    }

    public int PendingReaders
    {
        get { lock (_gate) return ActiveCount(_readers); }
    }

    public int PendingWriters
    {
        get { lock (_gate) return ActiveCount(_writers); }
    }

    public Exception? NotificationFailure
    {
        get { lock (_gate) return _notificationFailure; }
    }

    public Subscription SubscribeReadable(Action<Subscription> notify) =>
        Subscribe(WaitKind.Read, notify);

    public Subscription SubscribeWritable(Action<Subscription> notify) =>
        Subscribe(WaitKind.Write, notify);

    public bool Close()
    {
        var readers = CreateNotificationBatch();
        var writers = CreateNotificationBatch();
        lock (_gate)
        {
            if (_closed) return false;
            Capture(_readers, readers);
            Capture(_writers, writers);
            _closed = true;
        }
        readers.Dispatch();
        writers.Dispatch();
        return true;
    }

    public bool TryCommitRead(
        Subscription subscription,
        out ReadResult result,
        NotificationBatch notifications)
    {
        ArgumentNullException.ThrowIfNull(notifications);
        notifications.RequireEmpty();

        lock (_gate)
        {
            if (!Matches(subscription, WaitKind.Read))
            {
                result = new ReadResult(ReadStatus.WouldBlock);
                return false;
            }

            if (_count == 0)
            {
                if (!_closed)
                {
                    result = new ReadResult(ReadStatus.WouldBlock);
                    return false;
                }
                CloseLocked(_readers[subscription.Index]);
                result = new ReadResult(ReadStatus.Eof);
                return true;
            }

            if (_closed && _count == 1)
                Capture(_readers, notifications);
            else
                Capture(_writers, notifications);

            var value = _values[_head];
            _head = (_head + 1) % _values.Length;
            _count--;
            CloseLocked(_readers[subscription.Index]);
            result = new ReadResult(ReadStatus.Value, value);
            return true;
        }
    }

    public bool TryCommitWrite(
        Subscription subscription,
        long value,
        out WriteStatus result,
        NotificationBatch notifications)
    {
        ArgumentNullException.ThrowIfNull(notifications);
        notifications.RequireEmpty();

        lock (_gate)
        {
            if (!Matches(subscription, WaitKind.Write))
            {
                result = WriteStatus.WouldBlock;
                return false;
            }
            if (_closed)
            {
                CloseLocked(_writers[subscription.Index]);
                result = WriteStatus.Closed;
                return true;
            }
            if (_count == _values.Length)
            {
                result = WriteStatus.WouldBlock;
                return false;
            }

            Capture(_readers, notifications);
            _values[_tail] = value;
            _tail = (_tail + 1) % _values.Length;
            _count++;
            CloseLocked(_writers[subscription.Index]);
            result = WriteStatus.Written;
            return true;
        }
    }

    public bool TryWrite(long value)
    {
        var notifications = CreateNotificationBatch();
        lock (_gate)
        {
            if (_closed || _count == _values.Length) return false;
            Capture(_readers, notifications);
            _values[_tail] = value;
            _tail = (_tail + 1) % _values.Length;
            _count++;
        }
        notifications.Dispatch();
        return true;
    }

    public ReadResult TryReadResult()
    {
        var notifications = CreateNotificationBatch();
        ReadResult result;
        lock (_gate)
        {
            if (_count == 0)
                return new ReadResult(
                    _closed ? ReadStatus.Eof : ReadStatus.WouldBlock);

            if (_closed && _count == 1)
                Capture(_readers, notifications);
            else
                Capture(_writers, notifications);

            var value = _values[_head];
            _head = (_head + 1) % _values.Length;
            _count--;
            result = new ReadResult(ReadStatus.Value, value);
        }
        notifications.Dispatch();
        return result;
    }

    public bool TryRead(out long value)
    {
        var result = TryReadResult();
        if (result.Status == ReadStatus.Value)
        {
            value = result.Value;
            return true;
        }
        value = default;
        return false;
    }

    public NotificationBatch CreateNotificationBatch() =>
        new(this, _readers.Length);

    private Subscription Subscribe(
        WaitKind kind,
        Action<Subscription> notify)
    {
        ArgumentNullException.ThrowIfNull(notify);

        Subscription subscription;
        Action<Subscription>? immediate = null;
        lock (_gate)
        {
            var waiters = kind == WaitKind.Read ? _readers : _writers;
            var index = FindEmpty(waiters);
            if (index < 0)
                throw new InvalidOperationException(
                    "Channel select waiter capacity exhausted");
            if (_nextWaiterId == ulong.MaxValue)
                throw new OverflowException(
                    "Channel select waiter identity exhausted");

            var id = _nextWaiterId++;
            subscription = new Subscription(this, kind, index, id);
            var waiter = waiters[index];
            waiter.Id = id;
            waiter.Active = true;
            waiter.Notify = notify;

            var ready = kind == WaitKind.Read
                ? _count != 0 || _closed
                : _count != _values.Length || _closed;
            if (ready) immediate = notify;
        }

        if (immediate is not null)
        {
            try
            {
                immediate(subscription);
            }
            catch
            {
                _ = subscription.Close();
                throw;
            }
        }
        return subscription;
    }

    private void Capture(Waiter[] waiters, NotificationBatch notifications)
    {
        notifications.Attach(this);
        var kind = ReferenceEquals(waiters, _readers)
            ? WaitKind.Read : WaitKind.Write;
        for (var index = 0; index < waiters.Length; index++)
        {
            var waiter = waiters[index];
            if (!waiter.Active || waiter.Notify is null) continue;
            notifications.Add(waiter.Notify,
                new Subscription(this, kind, index, waiter.Id));
        }
    }

    private bool Matches(Subscription subscription, WaitKind expected)
    {
        if (!ReferenceEquals(subscription.Owner, this) ||
            subscription.Kind != expected)
            return false;

        var waiters = expected == WaitKind.Read ? _readers : _writers;
        if ((uint)subscription.Index >= (uint)waiters.Length) return false;
        var waiter = waiters[subscription.Index];
        return waiter.Active && waiter.Id == subscription.Id;
    }

    private bool Close(Subscription subscription)
    {
        lock (_gate)
        {
            if (!ReferenceEquals(subscription.Owner, this)) return false;
            var waiters = subscription.Kind == WaitKind.Read
                ? _readers : _writers;
            if ((uint)subscription.Index >= (uint)waiters.Length)
                return false;
            var waiter = waiters[subscription.Index];
            if (!waiter.Active || waiter.Id != subscription.Id)
                return false;
            CloseLocked(waiter);
            return true;
        }
    }

    private static void CloseLocked(Waiter waiter)
    {
        waiter.Active = false;
        waiter.Notify = null;
    }

    private static int FindEmpty(Waiter[] waiters)
    {
        for (var i = 0; i < waiters.Length; i++)
            if (!waiters[i].Active) return i;
        return -1;
    }

    private static int ActiveCount(Waiter[] waiters)
    {
        var count = 0;
        foreach (var waiter in waiters)
            if (waiter.Active) count++;
        return count;
    }

    private static Waiter[] CreateWaiters(int count)
    {
        var waiters = new Waiter[count];
        for (var i = 0; i < count; i++)
            waiters[i] = new Waiter();
        return waiters;
    }

    private void RecordNotificationFailure(Exception error)
    {
        lock (_gate)
            _notificationFailure ??= error;
    }

    public sealed class Subscription
    {
        private readonly SelectableI64Channel _owner;
        private readonly WaitKind _kind;
        private readonly int _index;
        private readonly ulong _id;

        internal Subscription(
            SelectableI64Channel owner,
            WaitKind kind,
            int index,
            ulong id)
        {
            _owner = owner;
            _kind = kind;
            _index = index;
            _id = id;
        }

        internal SelectableI64Channel Owner => _owner;
        internal WaitKind Kind => _kind;
        internal int Index => _index;
        internal ulong Id => _id;

        public bool Active
        {
            get
            {
                lock (_owner._gate)
                    return _owner.Matches(this, _kind);
            }
        }

        public bool Close() => _owner.Close(this);
    }

    /// <summary>
    /// Preallocated callback batch. Capture happens before channel mutation;
    /// dispatch happens after select/channel locks are released.
    /// </summary>
    public sealed class NotificationBatch
    {
        private readonly Action<Subscription>?[] _callbacks;
        private readonly Subscription?[] _subscriptions;
        private SelectableI64Channel? _owner;
        private int _count;

        internal NotificationBatch(SelectableI64Channel owner, int capacity)
        {
            _owner = owner;
            _callbacks = new Action<Subscription>?[capacity];
            _subscriptions = new Subscription?[capacity];
        }

        public int Count => _count;

        internal void RequireEmpty()
        {
            if (_count != 0)
                throw new InvalidOperationException(
                    "Channel notification batch must be empty");
        }

        internal void Attach(SelectableI64Channel owner)
        {
            if (!ReferenceEquals(_owner, owner))
                throw new InvalidOperationException(
                    "Notification batch belongs to another channel");
        }

        internal void Add(
            Action<Subscription> callback,
            Subscription subscription)
        {
            if (_count == _callbacks.Length)
                throw new InvalidOperationException(
                    "Channel notification batch overflow");
            _callbacks[_count] = callback;
            _subscriptions[_count] = subscription;
            _count++;
        }

        public void Dispatch()
        {
            Exception? first = null;
            for (var i = 0; i < _count; i++)
            {
                var callback = _callbacks[i];
                var subscription = _subscriptions[i];
                _callbacks[i] = null;
                _subscriptions[i] = null;
                try
                {
                    if (callback is not null && subscription is not null)
                        callback(subscription);
                }
                catch (Exception error)
                {
                    first ??= error;
                }
            }
            _count = 0;
            if (first is not null) _owner?.RecordNotificationFailure(first);
        }
    }
}
