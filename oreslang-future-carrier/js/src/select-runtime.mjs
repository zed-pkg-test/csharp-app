const MAX_SELECT_CAPACITY = 4096;

class SelectRegistration {
  #owner;
  #index;

  constructor(owner, index) {
    this.#owner = owner;
    this.#index = index;
  }

  get index() { return this.#index; }

  arm(revoke) {
    this.#owner._arm(this.#index, revoke);
  }

  tryWin(commit = () => true) {
    return this.#owner._tryWin(this.#index, commit);
  }

  get isWinner() {
    return this.#owner._isWinner(this.#index);
  }
}

class SelectCancellationHandle {
  #owner;

  constructor(owner) {
    this.#owner = owner;
  }

  cancel() {
    return this.#owner.cancel();
  }
}

/**
 * Source-independent select linearization primitive for the JS backend.
 *
 * JavaScript execution is run-to-completion within one isolate, so this object
 * does not pretend to provide cross-Worker shared-memory arbitration. It
 * provides the synchronous winner/cancellation contract needed by future
 * compiler-lowered Channel/Future adapters inside one actor/runtime lane.
 */
export class SelectArbiter {
  #slots;
  #terminal = 'pending';
  #winner = null;
  #commitInProgress = false;
  #cleanupFailure = null;

  constructor(capacity) {
    if (!Number.isSafeInteger(capacity) ||
        capacity < 1 || capacity > MAX_SELECT_CAPACITY) {
      throw new RangeError(
        'select capacity must be an integer in 1..' + MAX_SELECT_CAPACITY);
    }
    this.#slots = Array.from({length: capacity},
      () => ({state: 'empty', revoke: null}));
  }

  get capacity() { return this.#slots.length; }
  get winner() { return this.#terminal === 'won' ? this.#winner : null; }
  get cancelled() { return this.#terminal === 'cancelled'; }
  get cleanupFailure() { return this.#cleanupFailure; }

  registerCase(index) {
    this.#rejectCommitReentry();
    if (!Number.isSafeInteger(index) ||
        index < 0 || index >= this.#slots.length) {
      throw new RangeError('select case index out of range');
    }
    if (this.#terminal !== 'pending')
      throw new Error('cannot register case after select is terminal');
    const slot = this.#slots[index];
    if (slot.state !== 'empty')
      throw new Error('duplicate select case index');
    slot.state = 'pending';
    return new SelectRegistration(this, index);
  }

  cancellationHandle() {
    return new SelectCancellationHandle(this);
  }

  cancel() {
    this.#rejectCommitReentry();
    if (this.#terminal !== 'pending') return false;

    this.#terminal = 'cancelled';
    const callbacks = [];
    for (const slot of this.#slots) {
      if (slot.state !== 'pending') continue;
      if (slot.revoke !== null) {
        callbacks.push(slot.revoke);
        slot.revoke = null;
        slot.state = 'closed';
      } else {
        // The external readiness registration can still be returning its
        // revoker. arm() observes lost and invokes it immediately.
        slot.state = 'lost';
      }
    }
    this.#drain(callbacks);
    return true;
  }

  dispose() {
    this.cancel();
  }

  throwIfCleanupFailed() {
    if (this.#cleanupFailure !== null) throw this.#cleanupFailure;
  }

  _arm(index, revoke) {
    this.#rejectCommitReentry();
    if (typeof revoke !== 'function')
      throw new TypeError('select revoker must be a function');

    const slot = this.#slots[index];
    if (slot.state === 'pending') {
      if (slot.revoke !== null)
        throw new Error('select registration already armed');
      slot.revoke = revoke;
      return;
    }
    if (slot.state === 'winner') return;
    if (slot.state === 'lost') {
      slot.state = 'closed';
      try {
        revoke();
      } catch (error) {
        this.#recordCleanupFailure(error);
      }
      return;
    }
    if (slot.state === 'closed')
      throw new Error('select registration already closed');
    throw new Error('select registration was not admitted');
  }

  _tryWin(index, commit) {
    if (typeof commit !== 'function')
      throw new TypeError('select commit must be a function');
    this.#rejectCommitReentry();

    const slot = this.#slots[index];
    if (this.#terminal !== 'pending' || slot.state !== 'pending')
      return false;

    this.#commitInProgress = true;
    let committed;
    try {
      committed = commit();
    } finally {
      this.#commitInProgress = false;
    }
    if (typeof committed !== 'boolean') {
      throw new TypeError(
        'select commit must return a synchronous boolean; Promise/thenable commits are unsupported');
    }
    if (!committed) return false;

    this.#terminal = 'won';
    this.#winner = index;
    slot.state = 'winner';

    const callbacks = [];
    for (let i = 0; i < this.#slots.length; i++) {
      if (i === index) continue;
      const loser = this.#slots[i];
      if (loser.state !== 'pending') continue;
      if (loser.revoke !== null) {
        callbacks.push(loser.revoke);
        loser.revoke = null;
        loser.state = 'closed';
      } else {
        loser.state = 'lost';
      }
    }
    this.#drain(callbacks);
    return true;
  }

  _isWinner(index) {
    return this.#terminal === 'won' &&
      this.#winner === index &&
      this.#slots[index].state === 'winner';
  }

  #drain(callbacks) {
    let firstFailure = null;
    for (const callback of callbacks) {
      try {
        callback();
      } catch (error) {
        firstFailure ??= error instanceof Error ? error : new Error(String(error));
      }
    }
    if (firstFailure !== null) this.#recordCleanupFailure(firstFailure);
  }

  #recordCleanupFailure(error) {
    if (this.#cleanupFailure !== null) return;
    this.#cleanupFailure = error instanceof Error ? error : new Error(String(error));
  }

  #rejectCommitReentry() {
    if (this.#commitInProgress)
      throw new Error('select commit callback must not re-enter its arbiter');
  }
}
