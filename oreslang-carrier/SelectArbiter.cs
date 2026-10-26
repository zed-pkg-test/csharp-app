using System.Runtime.ExceptionServices;

namespace Ores.Net.Runtime;

/// <summary>
/// Runtime-only select linearization primitive. This does not enable source
/// select syntax and does not implement Channel&lt;T&gt; or Future&lt;T&gt;.
/// </summary>
public sealed class SelectArbiter : IDisposable
{
    private const int MaxCapacity = 4096;

    private enum Terminal
    {
        Pending,
        Won,
        Cancelled
    }

    private enum SlotState
    {
        Empty,
        Pending,
        Winner,
        Lost,
        Closed
    }

    private sealed class Slot
    {
        public SlotState State;
        public Action? Revoke;
    }

    private readonly object _gate = new();
    private readonly Slot[] _slots;
    private Terminal _terminal;
    private int _winner = -1;
    private bool _commitInProgress;
    private ExceptionDispatchInfo? _cleanupFailure;

    public SelectArbiter(int capacity)
    {
        if (capacity <= 0 || capacity > MaxCapacity)
            throw new ArgumentOutOfRangeException(nameof(capacity),
                $"Select capacity must be in 1..{MaxCapacity}");
        _slots = new Slot[capacity];
        for (var i = 0; i < _slots.Length; i++)
            _slots[i] = new Slot();
    }

    public int Capacity => _slots.Length;

    public int? Winner
    {
        get
        {
            lock (_gate)
                return _terminal == Terminal.Won ? _winner : null;
        }
    }

    public bool IsCancelled
    {
        get { lock (_gate) return _terminal == Terminal.Cancelled; }
    }

    public Exception? CleanupFailure
    {
        get { lock (_gate) return _cleanupFailure?.SourceException; }
    }

    public Registration RegisterCase(int index)
    {
        lock (_gate)
        {
            EnsureNoCommitReentry();
            if ((uint)index >= (uint)_slots.Length)
                throw new ArgumentOutOfRangeException(nameof(index));
            if (_terminal != Terminal.Pending)
                throw new InvalidOperationException(
                    "Cannot register a case after select is terminal");
            var slot = _slots[index];
            if (slot.State != SlotState.Empty)
                throw new InvalidOperationException("Duplicate select case index");
            slot.State = SlotState.Pending;
            return new Registration(this, index);
        }
    }

    public CancellationHandle GetCancellationHandle() => new(this);

    public bool Cancel()
    {
        // Allocate before the terminal transition. An allocation failure must
        // never leave a terminal select with undispatched loser cleanup.
        var callbacks = new Action?[_slots.Length];
        var count = 0;
        lock (_gate)
        {
            EnsureNoCommitReentry();
            if (_terminal != Terminal.Pending) return false;
            _terminal = Terminal.Cancelled;
            foreach (var slot in _slots)
            {
                if (slot.State != SlotState.Pending) continue;
                if (slot.Revoke is not null)
                {
                    callbacks[count++] = slot.Revoke;
                    slot.Revoke = null;
                    slot.State = SlotState.Closed;
                }
                else
                {
                    // External registration can still be returning its
                    // revoker. Arm() will see Lost and revoke immediately.
                    slot.State = SlotState.Lost;
                }
            }
        }
        Drain(callbacks, count);
        return true;
    }

    public void ThrowIfCleanupFailed()
    {
        ExceptionDispatchInfo? failure;
        lock (_gate) failure = _cleanupFailure;
        failure?.Throw();
    }

    public void Dispose()
    {
        _ = Cancel();
    }

    private bool TryWin(int index, Func<bool> commit)
    {
        ArgumentNullException.ThrowIfNull(commit);
        if ((uint)index >= (uint)_slots.Length) return false;

        // Allocate before executing the commit hook. If the hook mutates a
        // channel, no later allocation failure may make that mutation appear
        // to have happened without a terminal select winner.
        var callbacks = new Action?[_slots.Length];
        var count = 0;

        lock (_gate)
        {
            EnsureNoCommitReentry();
            if (_terminal != Terminal.Pending ||
                _slots[index].State != SlotState.Pending)
                return false;

            _commitInProgress = true;
            bool committed;
            try
            {
                committed = commit();
            }
            finally
            {
                _commitInProgress = false;
            }
            if (!committed) return false;

            _terminal = Terminal.Won;
            _winner = index;
            _slots[index].State = SlotState.Winner;

            for (var i = 0; i < _slots.Length; i++)
            {
                if (i == index) continue;
                var slot = _slots[i];
                if (slot.State != SlotState.Pending) continue;
                if (slot.Revoke is not null)
                {
                    callbacks[count++] = slot.Revoke;
                    slot.Revoke = null;
                    slot.State = SlotState.Closed;
                }
                else
                {
                    slot.State = SlotState.Lost;
                }
            }
        }

        Drain(callbacks, count);
        return true;
    }

    private void Arm(int index, Action revoke)
    {
        ArgumentNullException.ThrowIfNull(revoke);
        Action? immediate = null;
        lock (_gate)
        {
            EnsureNoCommitReentry();
            var slot = _slots[index];
            switch (slot.State)
            {
                case SlotState.Pending:
                    if (slot.Revoke is not null)
                        throw new InvalidOperationException(
                            "Select registration already armed");
                    slot.Revoke = revoke;
                    return;
                case SlotState.Winner:
                    // Winning producer consumed its registration; never revoke
                    // it as a loser.
                    return;
                case SlotState.Lost:
                    slot.State = SlotState.Closed;
                    immediate = revoke;
                    break;
                case SlotState.Closed:
                    throw new InvalidOperationException(
                        "Select registration already closed");
                case SlotState.Empty:
                    throw new InvalidOperationException(
                        "Select registration was not admitted");
            }
        }

        try
        {
            immediate!();
        }
        catch (Exception error)
        {
            RecordCleanupFailure(error);
        }
    }

    private bool IsWinner(int index)
    {
        lock (_gate)
            return _terminal == Terminal.Won &&
                   _winner == index &&
                   _slots[index].State == SlotState.Winner;
    }

    private void Drain(Action?[] callbacks, int count)
    {
        Exception? first = null;
        for (var i = 0; i < count; i++)
        {
            try
            {
                callbacks[i]?.Invoke();
            }
            catch (Exception error)
            {
                first ??= error;
            }
        }
        if (first is not null) RecordCleanupFailure(first);
    }

    private void RecordCleanupFailure(Exception error)
    {
        lock (_gate)
            _cleanupFailure ??= ExceptionDispatchInfo.Capture(error);
    }

    private void EnsureNoCommitReentry()
    {
        if (_commitInProgress)
            throw new InvalidOperationException(
                "Select commit callback must not re-enter its arbiter");
    }

    public sealed class Registration
    {
        private readonly SelectArbiter _owner;
        private readonly int _index;

        internal Registration(SelectArbiter owner, int index)
        {
            _owner = owner;
            _index = index;
        }

        public int Index => _index;

        public void Arm(Action revoke) => _owner.Arm(_index, revoke);

        public bool TryWin() => _owner.TryWin(_index, static () => true);

        /// <summary>
        /// Executes a bounded synchronous readiness/consume or readiness/publish
        /// step while the arbiter is still pending. The callback must not
        /// re-enter this arbiter and must return false without side effects when
        /// the operation cannot commit.
        /// </summary>
        public bool TryWin(Func<bool> commit) => _owner.TryWin(_index, commit);

        public bool IsWinner => _owner.IsWinner(_index);
    }

    public sealed class CancellationHandle
    {
        private readonly SelectArbiter _owner;

        internal CancellationHandle(SelectArbiter owner)
        {
            _owner = owner;
        }

        public bool Cancel() => _owner.Cancel();
    }
}
