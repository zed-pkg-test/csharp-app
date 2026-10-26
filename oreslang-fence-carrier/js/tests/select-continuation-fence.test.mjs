import test from 'node:test';
import assert from 'node:assert/strict';
import {SelectContinuationFence} from '../src/select-continuation-fence.mjs';

test('ticket is one shot',()=>{
  const fence=new SelectContinuationFence();
  const ticket=fence.register();
  assert.equal(fence.tryConsume(ticket),true);
  assert.equal(fence.tryConsume(ticket),false);
  assert.equal(fence.pendingTickets,0);
});

test('advance rejects an older queued ticket',()=>{
  const fence=new SelectContinuationFence();
  const old=fence.register(1);
  assert.equal(fence.advance(1),true);
  assert.equal(fence.generation,2);
  assert.equal(fence.tryConsume(old),false);
});

test('slot reuse keeps ticket identities distinct',()=>{
  const fence=new SelectContinuationFence({maxTickets:1});
  const old=fence.register();
  assert.equal(fence.advance(1),true);
  const current=fence.register(2);
  assert.notEqual(current.id,old.id);
  assert.equal(fence.tryConsume(old),false);
  assert.equal(fence.tryConsume(current),true);
});

test('closed ticket cannot deliver',()=>{
  const fence=new SelectContinuationFence();
  const ticket=fence.register();
  assert.equal(ticket.close(),true);
  assert.equal(ticket.close(),false);
  assert.equal(fence.tryConsume(ticket),false);
});

test('stale advance leaves current tickets intact',()=>{
  const fence=new SelectContinuationFence({generation:7});
  const ticket=fence.register(7);
  assert.equal(fence.advance(6),false);
  assert.equal(fence.generation,7);
  assert.equal(fence.tryConsume(ticket),true);
});

test('stale registration fails',()=>{
  const fence=new SelectContinuationFence({generation:3});
  assert.throws(()=>fence.register(2),/stale actor generation/);
  assert.equal(fence.pendingTickets,0);
});

test('ticket belongs to its fence',()=>{
  const first=new SelectContinuationFence();
  const second=new SelectContinuationFence();
  const ticket=first.register();
  assert.equal(second.tryConsume(ticket),false);
  assert.equal(ticket.close(),true);
});

test('delivery rechecks generation after queueing',()=>{
  const fence=new SelectContinuationFence();
  const queued=fence.register();
  assert.equal(fence.advance(1),true);
  let delivered=0;
  if(fence.tryConsume(queued))delivered++;
  assert.equal(delivered,0);
  const current=fence.register(2);
  if(fence.tryConsume(current))delivered++;
  assert.equal(delivered,1);
});
