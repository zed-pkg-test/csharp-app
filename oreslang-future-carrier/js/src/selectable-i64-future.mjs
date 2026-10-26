const I64_MIN=-(1n<<63n);
const I64_MAX=(1n<<63n)-1n;

class ResultCell {
  status='pending';
  value=0n;
  error=null;
  reset(){this.status='pending';this.value=0n;this.error=null;}
}

class Subscription {
  constructor(owner,index,id){this.owner=owner;this.index=index;this.id=id;}
  get active(){return this.owner._matches(this);}
  close(){return this.owner._close(this);}
}

export class SelectableI64Future {
  #status='pending';
  #value=0n;
  #error=null;
  #waiters;
  #nextId=1;
  #notificationFailure=null;

  constructor({maxWaiters=64}={}){
    if(!Number.isSafeInteger(maxWaiters)||maxWaiters<1||maxWaiters>4096)
      throw new RangeError('future maxWaiters must be in 1..4096');
    this.#waiters=Array.from({length:maxWaiters},
      ()=>({id:0,active:false,notify:null}));
  }

  get status(){return this.#status;}
  get notificationFailure(){return this.#notificationFailure;}
  get pendingWaiters(){return this.#waiters.reduce((n,w)=>n+(w.active?1:0),0);}

  createResultCell(){return new ResultCell();}

  subscribeReady(notify){
    if(typeof notify!=='function')throw new TypeError('future callback required');
    const index=this.#waiters.findIndex(w=>!w.active);
    if(index<0)throw new Error('future waiter capacity exhausted');
    if(this.#nextId>=Number.MAX_SAFE_INTEGER)
      throw new Error('future waiter identity exhausted');
    const sub=new Subscription(this,index,this.#nextId++);
    const waiter=this.#waiters[index];
    waiter.id=sub.id;waiter.active=true;waiter.notify=notify;
    if(this.#status!=='pending'){
      try{notify(sub);}catch(e){sub.close();throw e;}
    }
    return sub;
  }

  tryCommit(sub,cell){
    if(!(cell instanceof ResultCell))
      throw new TypeError('result cell must come from this runtime');
    cell.reset();
    if(!this._matches(sub)||this.#status==='pending')return false;
    this.#closeSlot(this.#waiters[sub.index]);
    cell.status=this.#status;
    cell.value=this.#value;
    cell.error=this.#error;
    return true;
  }

  resolve(value){
    if(typeof value!=='bigint'||value<I64_MIN||value>I64_MAX)
      throw new TypeError('future value must be signed i64 BigInt');
    return this.#complete('value',value,null);
  }

  fail(error){
    const e=error instanceof Error?error:new Error(String(error));
    return this.#complete('failed',0n,e);
  }

  cancel(){return this.#complete('cancelled',0n,null);}

  _matches(sub){
    if(!(sub instanceof Subscription)||sub.owner!==this||
       !Number.isSafeInteger(sub.index)||sub.index<0||
       sub.index>=this.#waiters.length)return false;
    const waiter=this.#waiters[sub.index];
    return waiter.active&&waiter.id===sub.id;
  }

  _close(sub){
    if(!this._matches(sub))return false;
    this.#closeSlot(this.#waiters[sub.index]);
    return true;
  }

  #complete(status,value,error){
    if(this.#status!=='pending')return false;
    const notices=[];
    for(let i=0;i<this.#waiters.length;i++){
      const w=this.#waiters[i];
      if(w.active&&w.notify!==null)
        notices.push([w.notify,new Subscription(this,i,w.id)]);
    }
    this.#status=status;this.#value=value;this.#error=error;
    let first=null;
    for(const [notify,sub] of notices){
      try{notify(sub);}catch(e){first??=e instanceof Error?e:new Error(String(e));}
    }
    this.#notificationFailure??=first;
    return true;
  }

  #closeSlot(waiter){waiter.active=false;waiter.notify=null;}
}
