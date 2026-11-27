const test=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
function fixture(){
  const html=fs.readFileSync(__dirname+'/index.html','utf8');
  const block=html.slice(html.indexOf('  let logCopyCodes = false;'),html.indexOf('  const logCtrl = (action)'));
  const ctx={logText:e=>e.textContent,setLogText:(e,t)=>{e.textContent=t;},logBody:{textContent:''},logMessage:{textContent:'',classList:{remove(){}}},logModal:{hidden:true},
    logDialogTitle:{},logSwitchButton:{setAttribute(){}},logSizeLabel:{},
    poller:{cancel(){},every(){return 1;}},reply:{ok:true},urls:[],
    api(url){this.urls.push(url);return Promise.resolve(this.reply);}};
  ctx.api=ctx.api.bind(ctx);vm.createContext(ctx);
  vm.runInContext(block+'\nglobalThis.logs={openLogModal,logTick,getOffset:()=>logOffset};',ctx);
  return ctx;
}
const settle=()=>new Promise(setImmediate);
test('an unclosed activation code is visible immediately and appended once',async()=>{
  const h=fixture();h.reply={ok:true,on:1,data:'绑定码 123',next:12,size:12,source:'/tmp/a',done:false};
  h.logs.openLogModal({kind:'app',name:'xz_watch.sh'});await settle();
  assert.equal(h.logBody.textContent,'绑定码 123');
  h.reply={...h.reply,data:'456',next:15};await h.logs.logTick();
  assert.equal(h.logBody.textContent,'绑定码 123456');
});
test('a fallback file switching to the declared file resets the cursor',async()=>{
  const h=fixture();h.reply={ok:true,data:'fallback',next:8,source:'/tmp/raw',note:'回退'};
  h.logs.openLogModal({kind:'app',name:'a.sh'});await settle();
  assert.equal(h.logMessage.textContent,'回退');
  h.reply={ok:true,data:'partial',next:20,source:'/tmp/custom'};await h.logs.logTick();
  assert.equal(h.logBody.textContent,'');assert.equal(h.logs.getOffset(),0);
  await h.logs.logTick();assert.match(h.urls.at(-1),/offset=0$/);
});
test('truncation renders the replacement and continues from its end',async()=>{
  const h=fixture();h.reply={ok:true,data:'old longer log',next:100,source:'/tmp/a'};
  h.logs.openLogModal({kind:'app',name:'a.sh'});await settle();
  h.reply={ok:true,data:'new',next:3,source:'/tmp/a',rotated:1};await h.logs.logTick();
  assert.equal(h.logBody.textContent,'new');assert.equal(h.logs.getOffset(),3);
});

test('batch console follows all scripts after exit, preserves cursors and bounds output',async()=>{
  const html=fs.readFileSync(__dirname+'/index.html','utf8');
  const block=html.slice(html.indexOf('  let consoleTimer = null'),html.indexOf('  /* ---------- CPU/内存：'));
  let tick,clear,reply={ok:true,entries:[{key:'door:0',name:'door_open.sh',cursor:'1',data:'opened\\n'}]};
  const calls=[];
  const ctx={logText:e=>e.textContent,setLogText:(e,t)=>{e.textContent=t;},TextEncoder,TextDecoder,consoleOutput:{textContent:''},consoleTarget:{},clearConsoleButton:{addEventListener(_,fn){clear=fn;}},
    poller:{cancel(){},every(fn){tick=fn;return 1;}},api(url,body){calls.push({url,body});return Promise.resolve(reply);}};
  vm.createContext(ctx);vm.runInContext(block+'\nglobalThis.open=openConsole;globalThis.stop=stopConsolePoll;',ctx);
  ctx.open();await settle();
  assert.match(ctx.consoleOutput.textContent,/\[door_open.sh\] opened/);
  reply={ok:true,entries:[{key:'door:0',name:'door_open.sh',cursor:'1',data:''},{key:'xz:0',name:'xz_watch.sh',cursor:'2',data:'hello'}]};
  await tick();assert.equal(calls.at(-1).body['door:0'],'1');
  assert.match(ctx.consoleOutput.textContent,/\[xz_watch.sh\] hello/);
  clear();await tick();assert.doesNotMatch(ctx.consoleOutput.textContent,/opened/);
  reply={ok:true,entries:[{key:'door:0',name:'door_open.sh',cursor:'3',reset:1,data:'opened again'}]};
  await tick();assert.match(ctx.consoleOutput.textContent,/opened again/);
  assert.equal(calls.at(-1).url,'/api/apps/logs');
  reply.entries[0].data='a'.repeat(150000);await tick();assert.ok(ctx.consoleOutput.textContent.length<=131072);
  reply.entries[0].data='中'.repeat(60000);await tick();
  assert.ok(Buffer.byteLength(ctx.consoleOutput.textContent)<=131072);
  assert.doesNotMatch(ctx.consoleOutput.textContent,/\ufffd/);
});
