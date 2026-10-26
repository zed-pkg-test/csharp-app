import test from 'node:test';
import assert from 'node:assert/strict';
import {SelectArbiter} from '../src/select-runtime.mjs';
import {SelectableI64Future} from '../src/selectable-i64-future.mjs';

function bind(future,registration,onResult,onCallback=()=>{}){
  return future.subscribeReady(sub=>{
    onCallback();
    const cell=future.createResultCell();
    const won=registration.tryWin(()=>future.tryCommit(sub,cell));
    if(won)onResult(cell);
  });
}

test('pending Future resolves and selected arm commits value',()=>{
  const future=new SelectableI64Future();
  const select=new SelectArbiter(1);
  const arm=select.registerCase(0);
  let result=null,callbacks=0;
  const wait=bind(future,arm,c=>{result={status:c.status,value:c.value};},
    ()=>{callbacks++;});
  arm.arm(()=>wait.close());
  assert.equal(select.winner,null);
  assert.equal(future.resolve(42n),true);
  assert.equal(select.winner,0);
  assert.equal(callbacks,1);
  assert.deepEqual(result,{status:'value',value:42n});
  assert.equal(future.pendingWaiters,0);
});

test('Future completion is immutable and non-consuming',()=>{
  const future=new SelectableI64Future();
  assert.equal(future.resolve(7n),true);
  assert.equal(future.resolve(8n),false);
  assert.equal(future.cancel(),false);

  for(let i=0;i<2;i++){
    const select=new SelectArbiter(1);
    const arm=select.registerCase(0);
    let value=0n;
    const wait=bind(future,arm,c=>{value=c.value;});
    arm.arm(()=>wait.close());
    assert.equal(select.winner,0);
    assert.equal(value,7n);
  }
});

test('stale same-Future notification cannot win losing case',()=>{
  const future=new SelectableI64Future();
  const select=new SelectArbiter(2);
  const first=select.registerCase(0);
  const second=select.registerCase(1);
  let firstCallbacks=0,secondCallbacks=0,sideEffects=0;
  const firstWait=bind(future,first,()=>{sideEffects++;},()=>{firstCallbacks++;});
  const secondWait=bind(future,second,()=>{sideEffects+=100;},()=>{secondCallbacks++;});
  first.arm(()=>firstWait.close());
  second.arm(()=>secondWait.close());

  assert.equal(future.resolve(9n),true);
  assert.equal(select.winner,0);
  assert.equal(firstCallbacks,1);
  assert.equal(secondCallbacks,1);
  assert.equal(sideEffects,1);
  assert.equal(future.pendingWaiters,0);
});

test('select cancellation revokes pending Future subscription',()=>{
  const future=new SelectableI64Future();
  const select=new SelectArbiter(1);
  const arm=select.registerCase(0);
  let callbacks=0;
  const wait=bind(future,arm,()=>{},()=>{callbacks++;});
  arm.arm(()=>wait.close());
  assert.equal(future.pendingWaiters,1);
  assert.equal(select.cancel(),true);
  assert.equal(future.pendingWaiters,0);
  assert.equal(future.resolve(5n),true);
  assert.equal(callbacks,0);
});

test('Future cancellation is a selectable terminal outcome',()=>{
  const future=new SelectableI64Future();
  const select=new SelectArbiter(1);
  const arm=select.registerCase(0);
  let status='';
  const wait=bind(future,arm,c=>{status=c.status;});
  arm.arm(()=>wait.close());
  assert.equal(future.cancel(),true);
  assert.equal(select.winner,0);
  assert.equal(status,'cancelled');
});

test('Future failure is a selectable terminal outcome',()=>{
  const future=new SelectableI64Future();
  const select=new SelectArbiter(1);
  const arm=select.registerCase(0);
  let result=null;
  const wait=bind(future,arm,c=>{result=c;});
  arm.arm(()=>wait.close());
  assert.equal(future.fail(new Error('boom')),true);
  assert.equal(select.winner,0);
  assert.equal(result.status,'failed');
  assert.match(result.error.message,/boom/);
});

test('notification failure is retained and waiter remains revocable',()=>{
  const future=new SelectableI64Future({maxWaiters:2});
  const wait=future.subscribeReady(()=>{throw new Error('notify failed');});
  assert.equal(future.resolve(1n),true);
  assert.match(future.notificationFailure?.message??'',/notify failed/);
  assert.equal(wait.close(),true);
  assert.equal(future.pendingWaiters,0);
});

test('foreign subscription and i64 bounds fail closed',()=>{
  const first=new SelectableI64Future();
  const second=new SelectableI64Future();
  const foreign=first.subscribeReady(()=>{});
  const cell=second.createResultCell();
  assert.equal(second.tryCommit(foreign,cell),false);
  assert.equal(foreign.close(),true);
  assert.throws(()=>second.resolve(1),/signed i64 BigInt/);
  assert.throws(()=>second.resolve(1n<<63n),/signed i64 BigInt/);
});
