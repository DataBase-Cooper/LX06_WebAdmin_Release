const test=require('node:test');
const assert=require('node:assert/strict');
const harness=require('./web_poll_harness.cjs');

test('all recurring panel work shares one timer, pending tasks do not overlap',async()=>{
  const h=harness(); let calls=0,finish;
  h.poll.poller.every(()=>{calls++;return new Promise(r=>finish=r);},2000);
  h.poll.startSystemStats();
  assert.equal(h.intervals,1);
  await h.advance(10000);
  assert.equal(calls,1);
  assert.equal(h.requests.length,1); // all device information shares core
  finish(); await h.advance(0); await h.advance(2000);
  assert.equal(calls,2);
  h.poll.stop();assert.equal(h.intervals,0);
});
test('hiding stops requests and timers; showing recovers; logout remains stopped',async()=>{
  const h=harness();h.poll.startSystemStats();
  const old=h.requests.slice();
  await h.visibility(true);assert.equal(h.intervals,0);
  assert.ok(old.every(r=>r.options.signal.aborted));
  await h.advance(20000);assert.equal(h.requests.length,1);
  await h.visibility(false);assert.equal(h.intervals,1);
  assert.equal(h.requests.length,2);
  h.poll.stop();await h.advance(30000);assert.equal(h.requests.length,2);
});
test('shared reads deduplicate timestamps, time out, and retry',async()=>{
  const h=harness();
  const a=h.poll.api('/api/wakeword?ts=1');const b=h.poll.api('/api/wakeword?ts=2');
  assert.equal(a,b);assert.equal(h.requests.length,1);
  await h.advance(15000);assert.equal((await a).ok,false);
  const c=h.poll.api('/api/wakeword?ts=3');assert.equal(h.requests.length,2);
  h.reply(h.requests[1],{ok:true});assert.equal((await c).ok,true);h.poll.stop();
});
test('late 401 from an earlier session cannot log out the new session',async()=>{
  const h=harness();const a=h.poll.api('/api/wakeword');
  h.requests[0].resolve({status:401,json:async()=>({ok:false})});
  h.poll.cancelReads();const b=h.poll.api('/api/wakeword');
  h.reply(h.requests[1],{ok:true});await a;await b;
  assert.equal(h.logins,0);h.poll.stop();
});
