// 在线升级 UI 回归（node:test，vm 沙箱执行 index.html 的 upgradeScript）。
// 覆盖：入口、两次原始流上传及回执、进度、接受→轮询
// （running/failed/succeeded→版本确认全链路）、取消、非对象 JSON、登出中断。
const test=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const html=fs.readFileSync(__dirname+'/index.html','utf8');
function harness(storage = new Map()){
  const source=html.match(/<script id="upgradeScript">([\s\S]*?)<\/script>/);
  assert.ok(source,'online-upgrade UI not implemented');
  const nodes=new Map(), requests=[], fetchQueue=[], fetched=[], timers=[];
  const node=id=>{
    if(!nodes.has(id))nodes.set(id,{hidden:false,disabled:false,checked:false,files:[],
      textContent:'',value:0,events:{},
      addEventListener(k,fn){this.events[k]=fn;},focus(){},click(){if(!this.disabled)this.events.click?.({target:this});}});
    return nodes.get(id);
  };
  class XHR{
    constructor(){this.upload={};this.headers={};requests.push(this);}
    open(method,path){this.method=method;this.path=path;}
    setRequestHeader(k,v){this.headers[k]=v;}
    send(body){this.body=body;}
    abort(){this.aborted=true;this.onabort?.();}
    reply(status,data){this.status=status;this.responseText=JSON.stringify(data);this.onload();}
  }
  const ctx={
    document:{getElementById:node,addEventListener(){}},
    XMLHttpRequest:XHR,
    AbortController,
    sessionStorage:{getItem:key=>storage.get(key),setItem:(key,value)=>storage.set(key,value)},
    fetch:url=>{
      fetched.push(url);
      /* 返回 Response 形状：脚本自己调 .json() */
      const next=fetchQueue.shift()||{ok:false,status:599,json:()=>Promise.reject(new Error('599'))};
      return Promise.resolve(next);
    },
    /* 测试期不挂真实定时器：轮询 tick 由测试手动驱动 */
    setInterval:(fn)=>{timers.push(fn);return timers.length;},
    clearInterval:id=>{if(id>=1&&id<=timers.length)timers[id-1]=null;},
    setTimeout:()=>0,
    clearTimeout(){}
  };
  vm.runInNewContext(source[1],ctx);
  return {node,requests,fetchQueue,fetched,timers,storage};
}
const ok=d=>({ok:true,status:200,json:()=>Promise.resolve(d)});
const unauthorized={ok:false,status:401};
const receipt='0123456789abcdef'.repeat(2);
const attempt='fedcba9876543210'.repeat(2);
const prior='11111111-1111-1111-1111-111111111111';
const fresh='22222222-2222-2222-2222-222222222222';
const accepted={ok:true,install_accepted:true,version:'V1',target:'boot1',wipe:'0',source_boot_id:prior};
const state=(version,running_slot,boot_id,source='image')=>ok({ok:true,version,running_slot,boot_id,source});
async function poll(h,index,response){
  h.fetchQueue.push(response);
  h.timers[index]();
  await new Promise(r=>setImmediate(r));
}
const prepare=(h,index)=>{
  const r=h.requests[index];
  assert.equal(r.path,'/api/upgrade/verify');
  assert.equal(r.headers['X-LX06-Upgrade'],'verify-v2');
  assert.equal(r.headers['X-LX06-Wipe'],undefined);
  r.reply(200,{ok:true,verified:true,version:'V1',receipt,attempt,source:'boot0',source_boot_id:prior});
  return h.requests[index+1];
};
test('entry button sits before logout; verify and stream before polling to success',async()=>{
  const h=harness();
  assert.ok(html.indexOf('id="firmwareUpgradeButton"')>0);
  assert.ok(html.indexOf('id="changePasswordButton"') < html.indexOf('id="firmwareUpgradeButton"'));
  assert.ok(html.indexOf('id="firmwareUpgradeButton"')<html.indexOf('id="logoutButton"'));
  assert.doesNotMatch(html.match(/<input[^>]*\bid="upgradeWipe"[^>]*>/)[0], /\bchecked\b/);
  /* 开弹窗会同步触发 loadSlotInfo 的一次 fetch：队列须先备好 */
  h.fetchQueue.push(ok({ok:true,run_version:'V0',boot:'boot0',data_free:4900000,ota:{state:'idle'}}));
  h.node('firmwareUpgradeButton').click();
  assert.equal(h.node('upgradeModal').hidden,false);
  await new Promise(r=>setImmediate(r));
  assert.match(h.node('upgradeSlotInfo').textContent,/当前版本 V0/);
  const file={name:'test.lx06ota',size:50000};
  h.node('upgradeFile').files=[file];
  h.node('upgradeWipe').checked=false;
  h.node('upgradeVerify').click();
  assert.equal(h.requests.length,1);
  assert.equal(h.requests[0].body,file);
  const r=prepare(h,0);
  assert.equal(r.path,'/api/upgrade/install');
  assert.equal(r.method,'POST');
  assert.equal(r.body,file);
  assert.equal(r.headers['X-LX06-Upgrade'],'install-v2');
  assert.equal(r.headers['X-LX06-Receipt'],receipt);
  assert.equal(r.headers['X-LX06-Wipe'],'0');
  assert.equal(r.headers['Content-Type'],'application/octet-stream');
  assert.equal(h.node('upgradeVerify').disabled,true);
  r.upload.onprogress({lengthComputable:true,loaded:25000,total:50000});
  assert.match(h.node('upgradeStatus').textContent,/上传固件 50%/);
  r.upload.onload();
  assert.match(h.node('upgradeStatus').textContent,/上传完成，设备正在处理固件/);
  h.node('upgradeClose').click();
  assert.equal(r.aborted,undefined,'closing during readback does not abort the update');
  r.reply(200,accepted);
  assert.match(h.node('upgradeStatus').textContent,/升级已受理 · boot1（保留用户数据）/);
  assert.equal(h.timers.length,1,'public boot confirmation started directly');
  await poll(h,0,state('V0','boot0',prior));
  assert.equal(h.fetched.at(-1),'/api/version');
  assert.doesNotMatch(h.node('upgradeStatus').textContent,/升级完成/);
  await poll(h,0,state('V1','boot1',fresh));
  assert.match(h.node('upgradeStatus').textContent,/升级完成，当前版本：V1/);
  assert.equal(h.node('upgradeVerify').disabled,false);
});
test('another open upgrade view confirms the boot through the public endpoint after its old session receives 401',async()=>{
  const h=harness();
  const running=ok({ok:true,boot_id:prior,run_version:'V0',ota:{state:'running',phase:'writing_rootfs',version:'V1',target:'boot1'}});
  h.fetchQueue.push(running,running);
  h.node('firmwareUpgradeButton').click();
  await new Promise(r=>setImmediate(r));
  assert.match(h.node('upgradeStatus').textContent,/正在写入根文件系统/);
  await poll(h,0,unauthorized);
  assert.equal(h.timers[0],null);
  assert.equal(h.timers.length,2);
  await poll(h,1,state('V1','boot1',fresh));
  assert.ok(h.fetched.includes('/api/version'));
  assert.match(h.node('upgradeStatus').textContent,/升级完成，当前版本：V1/);
});
test('same-version reinstall waits for a new boot, exact target version and actual mounted slot',async()=>{
  const h=harness();
  h.fetchQueue.push(ok({ok:true,boot_id:prior,run_version:'V1',ota:{state:'idle'}}));
  h.node('firmwareUpgradeButton').click();
  await new Promise(r=>setImmediate(r));
  h.node('upgradeFile').files=[{size:50000}];
  h.node('upgradeVerify').click();
  prepare(h,0).reply(200,accepted);
  for (const reply of [state('V1','boot1',prior),state('V2','boot1',fresh),
                       state('V1','boot0',fresh),state('V1','boot1',fresh,'build')]) {
    await poll(h,0,reply);
    assert.doesNotMatch(h.node('upgradeStatus').textContent,/升级完成/);
    assert.equal(h.node('upgradeVerify').disabled,true);
  }
  await poll(h,0,state('V1','boot1',fresh));
  assert.match(h.node('upgradeStatus').textContent,/升级完成，当前版本：V1/);
});
test('file selection shows the filename and verification requires no install',()=>{
  const h=harness(), file={name:'update.lx06ota',size:50000};
  h.node('upgradeFile').files=[file];
  h.node('upgradeFile').events.change();
  assert.equal(h.node('upgradeFileName').textContent,'update.lx06ota（49 KiB）');
  h.node('upgradeWipe').checked=true;
  h.node('upgradeCheck').click();
  const r=h.requests[0];
  assert.equal(r.path,'/api/upgrade/verify');
  assert.equal(r.body,file);
  assert.equal(r.headers['X-LX06-Upgrade'],'verify-v1');
  assert.equal(r.headers['X-LX06-Wipe'],undefined);
  r.reply(200,{ok:true,verified:true,version:'V1'});
  assert.equal(h.node('upgradeStatus').textContent,'校验通过 · V1');
  assert.equal(h.node('upgradeCheck').disabled,false);
  assert.equal(h.node('upgradeProgress').hidden,true);
  assert.equal(h.timers.length,0);
});
test('cancel prevents late success; rejected install cannot look installed',async()=>{
  const h=harness();
  h.node('upgradeFile').files=[{size:50000}];
  h.node('upgradeVerify').click();
  const r=h.requests[0];
  h.node('upgradeClose').click();
  assert.equal(r.aborted,true);
  assert.equal(h.node('upgradeModal').hidden,true);
  assert.equal(h.node('upgradeVerify').disabled,false,'cancel enables the upload button again');
  assert.equal(h.node('upgradeFileButton').disabled,false,'cancel enables file selection again');
  const message=h.node('upgradeStatus').textContent;
  r.reply(200,{ok:true,verified:true,version:'V1',receipt});
  assert.equal(h.requests.length,1,'late verification cannot start the second upload');
  assert.equal(h.node('upgradeStatus').textContent,message,'late reply ignored after cancel');
  h.node('upgradeModal').hidden=false;
  h.node('upgradeFile').files=[{size:50000}];
  h.node('upgradeVerify').click();
  h.requests[1].reply(422,{ok:false,error:'SHA校验失败'});
  assert.equal(h.node('upgradeStatus').textContent,'SHA校验失败');
  h.node('upgradeVerify').click();
  prepare(h,2).reply(507,{ok:false,error:'staging_full'});
  assert.equal(h.node('upgradeStatus').textContent,'临时空间不足，请稍后重试。');
  assert.equal(h.node('upgradeVerify').disabled,false);
});
test('non-object JSON responses release controls and allow retry',async()=>{
  for(const value of [null,[],'unexpected',true,42]){
    for(const code of [200,502]){
      const h=harness();
      h.node('upgradeFile').files=[{size:50000}];
      h.node('upgradeVerify').click();
      assert.doesNotThrow(()=>h.requests[0].reply(code,value));
      assert.equal(h.node('upgradeVerify').disabled,false);
      assert.equal(h.node('upgradeFile').disabled,false);
      assert.match(h.node('upgradeStatus').textContent,/设备返回异常/);
      h.node('upgradeVerify').click();
      assert.equal(h.requests.length,2,'retry after anomaly');
      prepare(h,1).reply(200,{...accepted,wipe:'1'});
      assert.match(h.node('upgradeStatus').textContent,/升级已受理 · boot1（清空用户数据）/);
    }
  }
});
test('a verification reply without a valid receipt cannot start installation',()=>{
  const h=harness();
  h.node('upgradeFile').files=[{size:50000}];
  h.node('upgradeVerify').click();
  h.requests[0].reply(200,{ok:true,verified:true,receipt:'abc'});
  assert.equal(h.requests.length,1);
  assert.equal(h.node('upgradeVerify').disabled,false);
  assert.equal(h.node('upgradeStatus').textContent,'无法完成升级校验，请稍后重试。');
});
test('logout aborts pending upload and no file produces no request',async()=>{
  const h=harness();
  h.node('upgradeVerify').click();
  assert.equal(h.requests.length,0,'no file, no request');
  h.node('upgradeFile').files=[{size:50000}];
  h.node('upgradeVerify').click();
  assert.equal(h.requests.length,1);
  h.node('logoutButton').click();
  assert.equal(h.requests[0].aborted,true);
  assert.equal(h.node('upgradeModal').hidden,true);
});

const otaStatus = (ota, boot_id = prior) => ok({ok:true,boot_id,run_version:'V0',ota});
const currentAttempt = {attempt,version:'V1',target:'boot1'};
function triggerInstall(h){
  h.node('upgradeFile').files=[{size:50000}];
  h.node('upgradeVerify').click();
  const request=prepare(h,0);
  request.upload.onprogress({lengthComputable:true,loaded:50000,total:50000});
  assert.equal(h.node('upgradeProgress').value,100);
  request.upload.onload();
  return request;
}
const settle = () => new Promise(r=>setImmediate(r));
test('loss of the install response tracks only this worker, then verifies the new boot without another POST',async()=>{
  const h=harness();
  const request=triggerInstall(h);
  h.fetchQueue.push(otaStatus({state:'failed',error:'upload_changed',...currentAttempt,attempt:receipt}));
  request.onerror();
  await settle();
  assert.match(h.node('upgradeStatus').textContent,/正在确认升级结果/);
  assert.doesNotMatch(h.node('upgradeLogText').value,/upload_changed/);
  assert.equal(h.node('upgradeVerify').disabled,true);
  h.node('upgradeClose').click();
  await poll(h,0,otaStatus({state:'running',phase:'verifying_rootfs',...currentAttempt}));
  assert.match(h.node('upgradeStatus').textContent,/正在读回校验根文件系统/);
  await poll(h,0,otaStatus({state:'succeeded',phase:'rebooting',...currentAttempt}));
  assert.equal(h.timers[0],null);
  await poll(h,1,state('V1','boot1',prior));
  assert.doesNotMatch(h.node('upgradeStatus').textContent,/升级完成/);
  await poll(h,1,state('V1','boot1',fresh));
  assert.match(h.node('upgradeStatus').textContent,/升级完成，当前版本：V1/);
  assert.equal(h.requests.length,2);
});
test('lost install response can confirm an already restarted device and rejects a rolled back boot',async()=>{
  for (const started of [true,false]) {
    const h=harness();
    const request=triggerInstall(h);
    h.fetchQueue.push(started ? unauthorized : otaStatus({state:'idle'},fresh));
    request.ontimeout();
    await settle();
    assert.equal(h.timers[0],null);
    await poll(h,1,started ? state('V1','boot1',fresh) : state('V0','boot0',fresh));
    assert.match(h.node('upgradeStatus').textContent,started ? /升级完成/ : /目标固件启动未确认/);
    if (!started) assert.match(h.node('upgradeLogText').value,/boot_target_mismatch/);
  }
});
test('failed write after a malformed acknowledgement surfaces the worker error and keeps a copyable log on refresh',async()=>{
  const h=harness();
  const request=triggerInstall(h);
  h.fetchQueue.push(otaStatus({state:'failed',phase:'verifying_rootfs',error:'rootfs_readback_mismatch',ts:1795752000,...currentAttempt}));
  request.reply(502,null);
  await settle();
  assert.match(h.node('upgradeStatus').textContent,/系统镜像写入后校验失败/);
  assert.equal(h.node('upgradeVerify').disabled,false);
  const log=h.node('upgradeLogText').value;
  assert.equal(h.node('upgradeLog').hidden,false);
  assert.match(log,/install \| invalid_response \| V1/);
  assert.match(log,/verifying_rootfs \| rootfs_readback_mismatch \| V1 \| boot1/);
  assert.doesNotMatch(log,new RegExp(receipt+'|'+attempt));
  assert.equal(harness(h.storage).node('upgradeLogText').value,log);
});
test('a gateway error in JSON still follows the actual install result',async()=>{
  const h=harness();
  const request=triggerInstall(h);
  h.fetchQueue.push(otaStatus({state:'succeeded',phase:'rebooting',...currentAttempt}));
  request.reply(502,{ok:false,error:'bad_gateway'});
  await settle();
  assert.match(h.node('upgradeLogText').value,/invalid_install_ack/);
  await poll(h,1,state('V1','boot1',fresh));
  assert.match(h.node('upgradeStatus').textContent,/升级完成/);
});
test('verify connection errors log the phase but do not start installing or polling',()=>{
  const h=harness();
  h.node('upgradeFile').files=[{size:50000}];
  h.node('upgradeVerify').click();
  h.requests[0].onerror();
  assert.match(h.node('upgradeStatus').textContent,/上传校验时连接中断/);
  assert.match(h.node('upgradeLogText').value,/verify \| connection_lost/);
  assert.equal(h.node('upgradeVerify').disabled,false);
  assert.equal(h.timers.length,0);
  assert.equal(h.requests.length,1);
});
test('a newly opened upgrade window recovers the latest device error and does not duplicate it',async()=>{
  const h=harness();
  const failed=otaStatus({state:'failed',phase:'writing_rootfs',error:'rootfs_write_failed',ts:1795752000,...currentAttempt});
  for (let i=0;i<2;i++){
    h.fetchQueue.push(failed);
    h.node('firmwareUpgradeButton').click();
    await settle();
  }
  assert.match(h.node('upgradeStatus').textContent,/上次升级失败：系统镜像写入失败/);
  assert.equal(h.node('upgradeLogText').value.match(/rootfs_write_failed/g).length,1);
});
test('unknown status has a polling limit, logs the outcome and releases the controls',async()=>{
  const h=harness();
  const request=triggerInstall(h);
  const idle=otaStatus({state:'idle'});
  h.fetchQueue.push(idle);
  request.onerror();
  await settle();
  for (let i=0;i<199;i++) await poll(h,0,idle);
  assert.equal(h.node('upgradeVerify').disabled,true);
  h.timers[0]();
  assert.equal(h.timers[0],null);
  assert.equal(h.node('upgradeVerify').disabled,false);
  assert.match(h.node('upgradeLogText').value,/upgrade_outcome_unknown/);
  assert.doesNotMatch(h.node('upgradeStatus').textContent,/升级完成/);
});
