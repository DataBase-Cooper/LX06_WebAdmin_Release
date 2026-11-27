const test=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const harness=require('./web_poll_harness.cjs');

test('one response paints every device panel and counts down before the next request',async()=>{
  const h=harness();h.poll.startCoreStats();
  assert.equal(h.node('coreRefreshButton').textContent,'刷新中…');
  h.reply(h.requests[0],{ok:true,cpu:42,memory:25,mem_total_kb:102400,mem_avail_kb:76800,
    tmp_total_kb:60000,tmp_used_kb:3000,data_total_kb:100000,data_used_kb:7000,
    core:{ssh:1,webd:1,engine:1,engine_state:'running',temp_c:51,keeper:1,safe_mode:0,user_apps:1},
    uptime:61,crash_pending:2});
  await h.advance(0);
  assert.equal(h.requests.length,1);
  assert.match(h.requests[0].url,/^\/api\/core\?/);
  assert.equal(h.node('cpuValue').textContent,'42%');
  assert.equal(h.node('memoryUsedLabel').textContent,'已用 25MB');
  assert.equal(h.node('tmpStorage').textContent,'60000:3000');
  assert.equal(h.node('dataStorage').textContent,'100000:7000');
  assert.equal(h.node('rowSsh').b.textContent,'运行中');
  assert.equal(h.node('rowCrash').b.textContent,'2 份快照');
  assert.equal(h.node('coreUptime').textContent,'运行 1分1秒');
  for(let i=5;i>0;i--){assert.equal(h.node('coreRefreshButton').textContent,`刷新连接（${i}秒）`);await h.advance(1000);}
  assert.equal(h.requests.length,2);h.poll.stop();
});

test('manual clicks and repeated starts share slow request; timeout recovers',async()=>{
  const h=harness();h.poll.startCoreStats();
  const p=h.poll.refreshCore();assert.equal(h.poll.refreshCore(),p);h.poll.startSystemStats();
  await h.advance(10000);assert.equal(h.requests.length,1);
  await h.advance(5000);assert.equal(h.requests[0].options.signal.aborted,true);
  await h.advance(5000);assert.equal(h.requests.length,2);h.poll.stop();
});

test('late body cannot overwrite a new session or clear its pending request',async()=>{
  const h=harness();let finish;
  h.poll.startCoreStats();h.requests[0].resolve({ok:true,status:200,json:()=>new Promise(r=>finish=r)});
  await h.advance(0);h.poll.stopCoreStats();h.poll.startCoreStats();
  finish({ok:true,cpu:99,crash_pending:9,core:{ssh:1}});await h.advance(0);
  assert.equal(h.node('rowSsh').b.textContent,'');
  await h.advance(10000);assert.equal(h.requests.length,2);
  h.reply(h.requests[1],{ok:true,cpu:20,memory:30,crash_pending:0,core:{ssh:0}});
  await h.advance(0);assert.equal(h.node('rowCrash').b.textContent,'无崩溃');
  assert.equal(h.node('cpuValue').textContent,'20%');h.poll.stop();
});

test('network and server failures release the cycle for retry',async()=>{
  const h=harness();h.poll.startCoreStats();h.requests[0].reject(Error('network'));
  await h.advance(0);await h.advance(5000);assert.equal(h.requests.length,2);
  h.reply(h.requests[1],{ok:false});await h.advance(0);await h.advance(5000);
  assert.equal(h.requests.length,3);h.poll.stop();
});

test('CPU and memory have no independent reconnect buttons',()=>{
  const html=fs.readFileSync(path.join(__dirname,'index.html'),'utf8');
  assert.doesNotMatch(html,/cpuRefreshButton|memoryRefreshButton|api\("\/api\/system-stats/);
  assert.equal((html.match(/id="coreRefreshButton"/g)||[]).length,1);
});
