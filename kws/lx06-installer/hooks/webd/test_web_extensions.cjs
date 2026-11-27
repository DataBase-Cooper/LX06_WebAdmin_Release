const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const html = fs.readFileSync(__dirname + '/index.html', 'utf8');
class Element {
  set textContent(v) {this._text=v;this.children=[];}
  get textContent() {return this._text||'';}
  set innerHTML(_) {throw new Error('Extension content must use inert DOM text');}
  set value(v) {
    this._value=String(v);
    if(this.tagName==='select')for(const option of this.children)option.selected=option.value===this._value;
  }
  get value() {return this.tagName==='select'?(this.children.find(o=>o.selected)?.value||''):this._value;}
  constructor(tag='div') { this.tagName=tag;this.children=[];this.hidden=false;this._value='';this.events={};this.type='';this.style={};this.dataset={}; }
  selectDefault() {
    if(this.tagName==='select'&&!this.children.some(o=>o.selected)) {
      const first=this.children.find(o=>!o.disabled);if(first)first.selected=true;
    }
  }
  append(...nodes) {this.children.push(...nodes);this.selectDefault();}
  replaceChildren(...nodes) {this.children=nodes;this.selectDefault();}
  addEventListener(k,f) {this.events[k]=f;}
  removeAttribute(k) {delete this[k];}
  setAttribute(k,v) {this[k]=String(v);}
  focus() {this.focused=true;}
  reportValidity() {return true;}
  querySelectorAll(selector) {
    return this.children.flatMap(c=>[c,...c.querySelectorAll(selector)]).filter(c=>['button','details'].includes(selector)?c.tagName===selector:true);
  }
}
function fixture() {
  const nodes=new Map(), urls=[], tasks=new Map(); let seq=0;
  const schema={version:1,title:'测试',description:'配置',fields:[
    {name:'model',type:'select',label:'模型',options:['auto','fast'],default:'fast'},
    {name:'key',type:'password',label:'口令'},
    {name:'on',type:'checkbox',label:'启用',default:'true'}
  ],actions:[{id:'login',label:'登录',fields:[]},{id:'save',label:'保存',fields:['model','key','on']}]};
  const ctx={codeCopyButton:()=>new Element('button'),URL,Map,Promise,TextEncoder,console,schema,urls,tasks,
    document:{createElement:t=>new Element(t),addEventListener(){}},
    $:id=>{if(!nodes.has(id))nodes.set(id,new Element());return nodes.get(id);},
    poller:{every(f){tasks.set(++seq,f);return seq;},cancel(id){tasks.delete(id);}},
    reply:{ok:true,running:false,exit:0,event:{message:'就绪'}},
  };
  ctx.api=async (url,body)=>{urls.push({url,body});if(body)return ctx.actionReply || {ok:true};return url.includes('/state?')?ctx.reply:{ok:true,schema};};
  vm.createContext(ctx);
  const code=html.slice(html.indexOf('  let tokenEntry ='),html.indexOf('  /* ---------- End script extension modal'));
  vm.runInContext(code+'\nglobalThis.ext={openExtension,closeExtension,extensionTick,extensionExecute,extensionRenderResult,inputs:extensionInputs};',ctx);
  return ctx;
}
const settle=()=>new Promise(setImmediate);

test('synthetic upload schedules greeting audio and the renderer exposes regeneration',async()=>{
  const {execFileSync}=require('node:child_process');
  const path=require('node:path');
  const root=path.resolve(__dirname,'../../runtime/model-adapter');
  const source="import sys,tempfile; from pathlib import Path; sys.path.insert(0,sys.argv[1]); import build_accounts; d=tempfile.TemporaryDirectory(); p=Path(d.name); build_accounts.build(p); sys.stdout.buffer.write((p/(sys.argv[2]+'_account.sh')).read_bytes())";
  for (const provider of ['chatgpt','grok']) {
    const script=execFileSync(process.env.PYTHON || 'python',['-c',source,root,provider],{encoding:'utf8'});
    let installed;
    const logs=[], scriptFile=new Element(), uploadButton=new Element();
    scriptFile.files=[{name:provider+'.sh',size:Buffer.byteLength(script),content:script}];
    const ctx={scriptFile,uploadButton,fileName:new Element(),uploadButtonText:new Element(),
      FormData:class {append(key,file){assert.equal(key,'file');this.file=file;}}, fetch:async(url,request)=>{assert.equal(url,'/api/apps?run=1');installed=request.body.file.content; return {status:200,json:async()=>({ok:true,name:provider+'.sh',size:500,data_free_kb:100,running:1,greeting_job:1})};},
      appendLog:s=>logs.push(s),formatBytes:String,formatSize:String,refreshStorage(){},consoleTick(){},setLogText(){},consoleOutput:new Element()};
    vm.createContext(ctx);
    vm.runInContext(html.slice(html.indexOf('  let pickedFile = null;'),html.indexOf('  /* ---------- 页面切换')),ctx);
    scriptFile.events.change(); await settle(); await settle();
    assert.equal(uploadButton.disabled,false);
    assert.ok(logs.some(s=>s.includes('部署成功')));
    assert.ok(logs.some(s=>s.includes('正在生成问候音频')));
    assert.equal(installed,script);
    const schema=JSON.parse(installed.split('# LX06-EXTENSION-BEGIN\n# ',2)[1].split('\n# LX06-EXTENSION-END',1)[0]);
    const h=fixture(); Object.assign(h.schema,schema);
    h.reply={ok:true,running:false,exit:0,event:{values:{},step:'configure'}};
    await h.ext.openExtension(provider+'.sh'); await settle(); await h.ext.extensionTick();
    const button=h.$('extensionActions').children.find(b=>b.extensionAction==='regenerate-greeting');
    assert.equal(button.textContent,'重新生成问候');
    assert.equal(button.hidden,false);
    await button.events.click();
    assert.equal(h.urls.filter(item=>item.body).at(-1).body.action,'regenerate-greeting');
  }
});

test('script sliders, segmented choices and switches share saved values and preserve drafts',async()=>{
  const h=fixture();h.schema.load='load';h.schema.actions[0].id='load';
  h.schema.fields.push({name:'gain',type:'number',label:'声音增益',widget:'slider',min:-12,max:12,step:.01,precision:2,center:0,unit:'dB',default:'0.00'});
  h.schema.fields[0].widget='segments';h.schema.fields[0].optionLabels=['自动','快速'];
  h.schema.fields[2].widget='switch';
  h.schema.conditions=[
    {id:'automatic',source:'settings.model',equals:'auto'},
    {id:'fast',source:'settings.model',equals:'fast'}
  ];
  h.schema.visibility=[
    {target:'fields.gain',conditions:['fast']},
    {target:'fields.on',conditions:['automatic']}
  ];
  h.reply={ok:true,running:false,exit:0,event:{values:{gain:'-2.75',model:'fast',on:'false'}}};
  await h.ext.openExtension('demo.sh');await settle();
  const gain=h.ext.inputs.get('gain'),mode=h.ext.inputs.get('model'),on=h.ext.inputs.get('on');
  const requests=h.urls.length;
  assert.equal(gain.extensionLabel.hidden,false);
  assert.equal(on.extensionLabel.hidden,true);
  assert.equal(gain.extensionRange.value,'-2.75');assert.equal(gain.step,'0.01');
  gain.extensionRange.value='3.25';gain.extensionRange.events.input();assert.equal(gain.value,'3.25');
  const preventDefault=()=>{};
  mode.extensionSegments[0].events.click({preventDefault});assert.equal(mode.value,'auto');
  assert.equal(gain.extensionLabel.hidden,true);
  assert.equal(on.extensionLabel.hidden,false);
  assert.equal(mode.extensionSegments[0]['aria-pressed'],'true');
  assert.equal(on.extensionToggle['aria-checked'],'false');on.extensionToggle.events.click({preventDefault});
  assert.equal(on.checked,true);assert.equal(on.extensionToggle['aria-checked'],'true');
  h.ext.extensionRenderResult({running:false,exit:0,event:{values:{gain:'-8.00',model:'fast',on:'false'}}});
  assert.equal(gain.value,'3.25');assert.equal(mode.value,'auto');assert.equal(on.checked,true);
  assert.equal(gain.extensionLabel.hidden,true);
  mode.extensionSegments[1].events.click({preventDefault});
  assert.equal(gain.extensionLabel.hidden,false);
  assert.equal(on.extensionLabel.hidden,true);
  assert.equal(h.urls.length,requests);
});
test('extension is before log, only declared scripts get a button',()=>{
  const block=html.slice(html.indexOf('if (a.extension === 1)'),html.indexOf('row.append(id, acts)'));
  assert.ok(block.indexOf('acts.append(xb)')<block.indexOf('acts.append(lb, b)'));
  assert.match(html,/id="extensionModal"[\s\S]*?class="password-dialog"/);
});
test('Xiaozhi end word is visible only when a pause is selected',async()=>{
  const h=fixture();
  h.schema.fields=[
    {name:'XZ_LISTEN_PAUSE_MS',type:'select',label:'预留换气时间',options:['0','1000','3000'],default:'0'},
    {name:'XZ_LISTEN_END_WORD',type:'text',label:'换气结束词',default:'发送'}
  ];
  h.schema.actions=[{id:'save',label:'保存',fields:['XZ_LISTEN_PAUSE_MS','XZ_LISTEN_END_WORD']}];
  h.schema.conditions=[{id:'pause-enabled',source:'settings.XZ_LISTEN_PAUSE_MS',notEquals:'0'}];
  h.schema.visibility=[{target:'fields.XZ_LISTEN_END_WORD',conditions:['pause-enabled']}];
  await h.ext.openExtension('xz_watch.sh');await settle();
  const pause=h.ext.inputs.get('XZ_LISTEN_PAUSE_MS'),word=h.ext.inputs.get('XZ_LISTEN_END_WORD');
  assert.equal(word.extensionLabel.hidden,true);
  assert.equal(word.value,'发送');
  for(const value of ['1000','3000','0']){
    pause.value=value;
    pause.events.change();
    assert.equal(word.extensionLabel.hidden,value==='0');
  }
  assert.equal(word.value,'发送');
});
test('generic settings conditions drive visibility without saving',async()=>{
  const h=fixture();
  h.schema.fields=[
    {name:'delivery',type:'select',label:'回执方式',options:['none','voice','text'],default:'none'},
    {name:'message',type:'text',label:'回执内容',default:''}
  ];
  h.schema.actions=[{id:'save',label:'保存',fields:['delivery','message']}];
  h.schema.conditions=[{id:'send-result',source:'settings.delivery',notEquals:'none'}];
  h.schema.visibility=[{target:'fields.message',conditions:['send-result']}];
  await h.ext.openExtension('work_order.sh');await settle();
  const delivery=h.ext.inputs.get('delivery'),message=h.ext.inputs.get('message');
  const requests=h.urls.length;
  for(const value of ['none','voice','text','none']){
    delivery.value=value;
    delivery.events.change();
    assert.equal(message.extensionLabel.hidden,value==='none');
  }
  assert.equal(h.urls.length,requests);
});
test('generic mode switches display only the declared branch settings',async()=>{
  const h=fixture();
  h.schema.fields=[
    {name:'interrupt_mode',type:'select',label:'打断方式',widget:'segments',options:['word','speech'],default:'word'},
    {name:'interrupt_phrase',type:'text',label:'打断词',default:'等等'},
    {name:'voice_threshold',type:'number',label:'音量门槛',default:'1000',min:0,max:8000},
    {name:'echo_control',type:'select',label:'回声保护',options:['0','1'],default:'0'}
  ];
  h.schema.actions=[{id:'save',label:'保存',fields:h.schema.fields.map(({name})=>name)}];
  h.schema.conditions=[
    {id:'word-only',source:'settings.interrupt_mode',equals:'word'},
    {id:'speech-only',source:'settings.interrupt_mode',equals:'speech'}
  ];
  h.schema.visibility=[
    {target:'fields.interrupt_phrase',conditions:['word-only']},
    {target:'fields.voice_threshold',conditions:['speech-only']},
    {target:'fields.echo_control',conditions:['speech-only']}
  ];
  await h.ext.openExtension('custom_voice.sh');await settle();
  const mode=h.ext.inputs.get('interrupt_mode');
  const word=h.ext.inputs.get('interrupt_phrase'), volume=h.ext.inputs.get('voice_threshold');
  const echo=h.ext.inputs.get('echo_control'), requests=h.urls.length;
  const preventDefault=()=>{};
  for(const index of [0,1,0]){
    mode.extensionSegments[index].events.click({preventDefault});
    assert.equal(word.extensionLabel.hidden,index===1);
    assert.equal(volume.extensionLabel.hidden,index===0);
    assert.equal(echo.extensionLabel.hidden,index===0);
  }
  assert.equal(h.urls.length,requests);
});
test('modal builds controls and sends only fields declared for selected action',async()=>{
  const h=fixture();await h.ext.openExtension('demo.sh',new Element());await settle();
  assert.equal(h.ext.inputs.get('model').value,'fast');
  h.ext.inputs.get('key').value='private';
  await h.ext.extensionExecute(h.schema.actions[1]);await settle();
  const body=h.urls.find(x=>x.body).body;
  assert.deepEqual(JSON.parse(JSON.stringify(body)),{name:'demo.sh',action:'save',values:{model:'fast',key:'private',on:'true'}});
  assert.equal(h.ext.inputs.get('key').value,'');
  await h.ext.extensionExecute(h.schema.actions[0]);await settle();
  assert.equal(Object.keys(h.urls.filter(x=>x.body).at(-1).body.values).length,0);
});
test('script-owned steps hide unrelated controls, wait for full load, and transition after login',async()=>{
  const h=fixture();
  h.schema.load='load';h.schema.actions.push({id:'load',label:'读取',fields:[]});
  h.schema.flow={initial:'login',steps:[
    {id:'login',title:'登录',fields:['key'],actions:['login']},
    {id:'configure',title:'设置',fields:['model','on'],actions:['save']}]};
  h.reply={ok:true,running:false,exit:0,event:{}};
  await h.ext.openExtension('flow.sh');await settle();
  assert.equal(h.$('extensionForm').hidden,true);
  h.reply={ok:true,running:false,exit:0,event:{step:'login',values:{model:'fast',on:'true'}}};
  await h.ext.extensionTick();
  assert.equal(h.$('extensionForm').hidden,false);
  assert.equal(h.ext.inputs.get('model').extensionLabel.hidden,true);
  assert.equal(h.ext.inputs.get('key').extensionLabel.hidden,false);
  h.ext.extensionRenderResult({running:false,exit:0,event:{step:'configure',message:'已登录'}});
  assert.equal(h.ext.inputs.get('model').extensionLabel.hidden,false);
  assert.equal(h.ext.inputs.get('key').extensionLabel.hidden,true);
  assert.equal(h.$('extensionActions').children.find(b=>b.extensionAction==='login').hidden,true);
});
test('failed workflow load exposes retry, and closing cancels step watches',async()=>{
  const h=fixture();h.schema.load='login';
  h.schema.flow={initial:'wait',steps:[{id:'wait',title:'配对',fields:[],actions:['login'],watch:'login',interval:8}]};
  h.reply={ok:true,running:false,exit:1,event:{message:'network'}};
  await h.ext.openExtension('flow.sh');await settle();await h.ext.extensionTick();
  assert.equal(h.$('extensionRetry').hidden,false);
  assert.equal(h.$('extensionLoading').hidden,true);
  h.reply={ok:true,running:false,exit:0,event:{step:'wait',values:{model:'fast',on:'true'}}};
  await h.ext.extensionTick();assert.ok(h.tasks.size);
  h.ext.closeExtension();assert.equal(h.tasks.size,0);
});
test('outputs are inert text; links reject javascript and credentials',()=>{
  const h=fixture();
  for(const link of ['javascript:alert(1)','https://name:secret@example.com','http://example.com']) {
    h.ext.extensionRenderResult({event:{message:'<img onerror=x>',link,code:'ABC'},running:true});
    assert.equal(h.$('extensionLink').hidden,true);
    assert.equal(h.$('extensionMessage').textContent,'<img onerror=x>');
  }
  h.ext.extensionRenderResult({running:true,event:{link:'https://auth.example.com/'}});
  assert.equal(h.$('extensionLink').hidden,false);
});
test('authorization link is prominent and usable while other actions are locked',async()=>{
  const h=fixture();h.reply={ok:true,running:true,event:{message:'等待授权',link:'https://auth.example.com/login'}};
  await h.ext.openExtension('demo.sh');await settle();
  assert.ok(h.$('extensionActions').children.every(b=>b.disabled));
  assert.equal(h.$('extensionLink').hidden,false);assert.equal(h.$('extensionLink').href,'https://auth.example.com/login');
  assert.equal(h.$('extensionPhase').textContent,'等待授权');
  assert.ok(html.indexOf('id="extensionLink"')<html.indexOf('id="extensionFields"'));
  h.reply={ok:true,running:false,exit:124,event:{link:'https://auth.example.com/expired'}};
  await h.ext.extensionTick();assert.equal(h.$('extensionLink').hidden,true);
  assert.ok(h.$('extensionActions').children.every(b=>!b.disabled));
});
test('failed status fetch unlocks actions and manual refresh recovers',async()=>{
  const h=fixture();h.reply={ok:false,error:'network'};
  await h.ext.openExtension('demo.sh');await settle();
  assert.ok(h.$('extensionActions').children.every(b=>!b.disabled));
  h.reply={ok:true,running:true,event:{link:'https://auth.example.com/login'}};
  await h.ext.extensionTick();assert.equal(h.$('extensionLink').hidden,false);
  assert.equal(h.tasks.size,1);
});
test('authorization codes have copy while waiting; live script policy refreshes and completion clears code',async()=>{
  const h=fixture();
  h.reply={ok:true,copy_codes:1,running:true,event:{link:'https://auth.example.com',code:'001234'}};
  await h.ext.openExtension('grok_account.sh');await settle();
  assert.equal(h.$('extensionCode').textContent,'验证码：001234');
  assert.equal(h.$('extensionCode').children.length,1);
  const copy = h.$('extensionCode').children[0];
  copy.textContent='已复制';
  await h.ext.extensionTick();
  assert.equal(h.$('extensionCode').children[0],copy);
  assert.equal(copy.textContent,'已复制');
  assert.ok(!h.$('extensionCode').children[0].disabled);
  assert.ok(h.$('extensionActions').children.every(b=>b.disabled));
  h.reply.copy_codes=0;await h.ext.extensionTick();
  assert.equal(h.$('extensionCode').children.length,0);
  h.reply={ok:true,copy_codes:1,running:false,exit:0,event:{message:'已绑定'}};
  await h.ext.extensionTick();assert.equal(h.$('extensionCode').hidden,true);
  assert.equal(h.$('extensionCode').children.length,0);
});
test('polling only while running and closing clears secrets',async()=>{
  const h=fixture();h.reply={ok:true,running:true,event:{message:'等待登录'}};
  await h.ext.openExtension('demo.sh');await settle();assert.equal(h.tasks.size,1);
  h.ext.inputs.get('key').value='private';h.ext.closeExtension();
  assert.equal(h.tasks.size,0);assert.equal(h.ext.inputs.size,0);
  assert.equal(h.$('extensionFields').children.length,0);
});
test('late schema and status cannot repopulate a closed modal',async()=>{
  const h=fixture();let resolve;
  h.api=()=>new Promise(r=>resolve=r);
  const pending=h.ext.openExtension('old.sh');h.ext.closeExtension();
  resolve({ok:true,schema:h.schema});await pending;
  assert.equal(h.ext.inputs.size,0);assert.equal(h.$('extensionModal').hidden,true);
});

test('settings auto-load, neutral buttons, no stale or password readback and draft preservation',async()=>{
  const h=fixture();h.schema.load='load';h.schema.actions[0].id='load';
  h.reply={ok:true,running:false,exit:0,event:{values:{model:'auto',key:'never-render'}}};
  await h.ext.openExtension('settings.sh');await settle();
  assert.equal(h.urls.find(x=>x.body).body.action,'load');
  assert.equal(h.ext.inputs.get('model').value,'auto');
  assert.equal(h.ext.inputs.get('key').value,'');
  assert.ok(h.$('extensionActions').children.every(b=>{
    const classes=b.className.split(/\s+/);
    return classes.includes('dialog-button') && classes.includes('dialog-button-secondary') &&
      !classes.includes('dialog-button-primary');
  }));
  h.ext.inputs.get('model').value='fast';h.ext.inputs.get('model').events.input();
  await h.ext.extensionTick();assert.equal(h.ext.inputs.get('model').value,'fast');
  h.reply={ok:true,running:true,event:{message:'loading'}};
  await h.ext.extensionExecute(h.schema.actions[0]);await settle();
  h.ext.inputs.get('model').value='fast';h.ext.inputs.get('model').events.input();
  h.reply={ok:true,running:false,exit:0,event:{values:{model:'auto'}}};
  await h.ext.extensionTick();assert.equal(h.ext.inputs.get('model').value,'fast');
});

test('failed initial read cannot save defaults over existing settings',async()=>{
  const h=fixture();h.schema.load='load';h.schema.actions[0].id='load';
  h.reply={ok:true,running:false,exit:2,event:{message:'read failed'}};
  await h.ext.openExtension('settings.sh');await settle();
  assert.equal(h.$('extensionActions').children[0].disabled,false);
  assert.equal(h.$('extensionActions').children[1].disabled,true);
});

test('UTF-8 field bounds stop an oversized request before submission',async()=>{
  const h=fixture();h.schema.fields[1].maxBytes=5;
  await h.ext.openExtension('limits.sh');await settle();
  h.ext.inputs.get('key').value='汉字';
  await h.ext.extensionExecute(h.schema.actions[1]);
  assert.equal(h.urls.filter(x=>x.body).length,0);
  assert.match(h.$('extensionMessage').textContent,/最多 5 字节/);
});

function dynamicFixture() {
  const h=fixture();
  h.schema.fields[0]={name:'model',type:'select',dynamic:true,label:'模型',options:['seed'],default:'old-model'};
  return h;
}
const modelOptions=h=>h.ext.inputs.get('model').children;

test('unverified account interruption is read-only and mode-bearing actions cannot submit',async()=>{
  const h=fixture();
  h.schema.fields[0]={name:'MODEL_BARGE_ANY',type:'select',label:'打断方式',options:['0','1'],widget:'segments',optionLabels:['打断词','人声']};
  h.schema.actions[1].fields=['MODEL_BARGE_ANY','key','on'];
  h.reply={ok:true,running:false,exit:0,event:{values:{MODEL_BARGE_ANY:'1'}}};
  await h.ext.openExtension('static.sh');await settle();
  const field=h.ext.inputs.get('MODEL_BARGE_ANY');
  assert.equal(field.extensionLabel.textContent,'打断方式：未验证');
  assert.equal(field.extensionSegments,undefined);
  assert.equal(h.$('extensionActions').children[1].disabled,true);
  await h.ext.extensionExecute(h.schema.actions[1]);
  assert.equal(h.urls.filter(x=>x.body).length,0);
});

test('declared mode env alias is read-only and cannot submit',async()=>{
  const h=fixture();
  h.schema.fields[0]={name:'mode_alias',env:'MODEL_BARGE_ANY',type:'select',label:'打断方式',options:['0','1']};
  h.schema.actions[1].fields=['mode_alias','key','on'];
  await h.ext.openExtension('static.sh');await settle();
  assert.equal(h.ext.inputs.get('mode_alias').extensionLabel.textContent,'打断方式：未验证');
  assert.equal(h.$('extensionActions').children[1].disabled,true);
  await h.ext.extensionExecute(h.schema.actions[1]);
  assert.equal(h.urls.filter(x=>x.body).length,0);
});

test('loaded unverified mode stays unknown while independent actions remain usable',async()=>{
  const h=fixture(); h.schema.load='load'; h.schema.actions[0].id='load';
  h.schema.fields[0]={name:'MODEL_BARGE_ANY',type:'select',label:'打断方式',options:['0','1'],default:'1'};
  h.schema.actions[1].fields=['MODEL_BARGE_ANY','key'];
  h.schema.actions.push({id:'independent',label:'独立操作',fields:[]});
  h.reply={ok:true,running:false,exit:0,event:{values:{MODEL_BARGE_ANY:'0'}}};
  await h.ext.openExtension('synthetic.sh');await settle();
  assert.equal(h.ext.inputs.get('MODEL_BARGE_ANY').extensionLabel.textContent,'打断方式：未验证');
  assert.equal(h.$('extensionActions').children[1].disabled,true);
  assert.equal(h.$('extensionActions').children[2].disabled,false);
  await h.ext.extensionExecute(h.schema.actions[2]);await settle();
  assert.equal(h.urls.filter(x=>x.body).at(-1).body.action,'independent');
});

test('dynamic model catalogue supports 64 labels and 12 neutral actions without replacing a saved old ID',async()=>{
  const h=dynamicFixture();h.schema.load='load';h.schema.actions[0].id='load';
  for(let i=2;i<12;i++)h.schema.actions.push({id:'action'+i,label:'操作'+i,fields:[]});
  const options=Array.from({length:64},(_,i)=>({value:'model-'+i,label:'模型 '+i}));
  h.reply={ok:true,running:false,exit:0,options:{model:options},values:{model:'retired-user-model'},event:{message:'模型目录已加载'}};
  await h.ext.openExtension('models.sh');await settle();
  const select=h.ext.inputs.get('model');assert.equal(select.tagName,'select');
  assert.equal(select.value,'retired-user-model');
  assert.equal(modelOptions(h).filter(o=>o.value.startsWith('model-')).length,64);
  assert.equal(modelOptions(h).find(o=>o.value==='model-63').textContent,'模型 63');
  assert.ok(modelOptions(h).some(o=>o.value==='seed'));
  assert.ok(modelOptions(h).some(o=>o.value==='retired-user-model'));
  assert.equal(h.$('extensionActions').children.length,12);
  assert.ok(h.$('extensionActions').children.every(b=>b.className.includes('dialog-button-secondary')));
  await h.ext.extensionExecute(h.schema.actions[1]);await settle();
  assert.equal(h.urls.filter(x=>x.body).at(-1).body.values.model,'retired-user-model');
});

test('dynamic options never select the first model or permit a hand-entered ID',async()=>{
  const h=dynamicFixture();delete h.schema.fields[0].default;h.schema.fields[0].options=[];
  h.reply={ok:true,running:false,exit:0,options:{model:[{value:'available',label:'可用模型'}]},event:{}};
  await h.ext.openExtension('models.sh');await settle();
  const select=h.ext.inputs.get('model');assert.equal(select.value,'');
  assert.equal(modelOptions(h)[0].textContent,'请选择模型');
  await h.ext.extensionExecute(h.schema.actions[1]);assert.equal(h.urls.filter(x=>x.body).length,0);
  assert.match(h.$('extensionMessage').textContent,/请先选择模型/);
  select.value='invented';assert.equal(select.value,'');
  await h.ext.extensionExecute(h.schema.actions[1]);assert.equal(h.urls.filter(x=>x.body).length,0);
  select.value='available';select.events.change();
  await h.ext.extensionExecute(h.schema.actions[1]);await settle();
  assert.equal(h.urls.find(x=>x.body).body.values.model,'available');
});

test('multi-line load uses committed values when its final event contains only model options',async()=>{
  const h=dynamicFixture();h.schema.load='load';h.schema.actions[0].id='load';
  h.reply={ok:true,running:false,exit:0,values:{model:'saved-id',key:'secret-never-readback'},
    options:{model:[{value:'new-id',label:'新模型'}]},event:{message:'模型已获取',options:{model:[{value:'new-id',label:'新模型'}]}}};
  await h.ext.openExtension('models.sh');await settle();
  assert.equal(h.ext.inputs.get('model').value,'saved-id');
  assert.equal(h.ext.inputs.get('key').value,'');
  assert.equal(h.$('extensionActions').children[1].disabled,false);
  assert.ok(modelOptions(h).some(o=>o.value==='new-id'));
});

test('in-flight catalogue updates preserve drafts and ignore unfinished event options',async()=>{
  const h=dynamicFixture();h.schema.load='load';h.schema.actions[0].id='load';
  const old=[{value:'draft-id',label:'草稿模型'}];
  h.reply={ok:true,running:false,exit:0,options:{model:old},event:{values:{model:'old-model'}}};
  await h.ext.openExtension('models.sh');await settle();
  h.reply={ok:true,running:true,options:{model:old},event:{options:{model:[{value:'not-committed',label:'尚未完成'}]}}};
  await h.ext.extensionExecute(h.schema.actions[0]);await settle();
  const select=h.ext.inputs.get('model');select.value='draft-id';select.events.input();
  assert.ok(!modelOptions(h).some(o=>o.value==='not-committed'));
  const retained=modelOptions(h).find(o=>o.value==='draft-id');
  await h.ext.extensionTick();assert.equal(modelOptions(h).find(o=>o.value==='draft-id'),retained);
  h.reply={ok:true,running:false,exit:0,options:{model:[{value:'new-first',label:'新目录首项'}]},
    values:{model:'old-model'},event:{message:'目录已更新'}};
  await h.ext.extensionTick();
  assert.equal(select.value,'draft-id');
  assert.ok(modelOptions(h).some(o=>o.value==='new-first'));
  assert.ok(modelOptions(h).some(o=>o.value==='draft-id'));
  assert.equal(h.$('extensionActions').children[1].disabled,false);
});

test('empty catalogue keeps seeds and current model while failed refresh changes neither',async()=>{
  const h=dynamicFixture();h.reply={ok:true,running:false,exit:0,event:{options:{model:[{value:'old-model',label:'旧模型'}]}}};
  await h.ext.openExtension('models.sh');await settle();
  h.reply={ok:true,running:false,exit:1,values:{model:'bad'},event:{options:{model:[{value:'bad',label:'失败结果'}]}}};
  await h.ext.extensionTick();assert.equal(h.ext.inputs.get('model').value,'old-model');
  assert.ok(!modelOptions(h).some(o=>o.value==='bad'));
  const before=modelOptions(h);
  h.reply={ok:false,error:'连接失败',options:{model:[{value:'bad',label:'错误目录'}]}};
  await h.ext.extensionTick();assert.equal(modelOptions(h),before);
  h.reply={ok:true,running:false,exit:0,event:{options:{model:[]}}};
  await h.ext.extensionTick();
  assert.equal(h.ext.inputs.get('model').value,'old-model');
  assert.deepEqual(modelOptions(h).map(o=>o.value),['seed','old-model']);
});

test('malformed catalogues are rejected atomically and model labels remain inert text',async()=>{
  const h=dynamicFixture();
  h.reply={ok:true,running:false,exit:0,options:{model:[{value:'literal-id',label:'<img src=x onerror=alert(1)>'}]},event:{}};
  await h.ext.openExtension('models.sh');await settle();
  assert.equal(modelOptions(h).find(o=>o.value==='literal-id').textContent,'<img src=x onerror=alert(1)>');
  const before=modelOptions(h);
  const good={value:'other',label:'另一个模型'};
  const invalid=[
    {model:Array.from({length:65},(_,i)=>({value:String(i),label:String(i)}))},
    {model:[good,good]}, {model:[{value:'x',label:'汉'.repeat(54)}]},
    {model:[{value:'x\nheader',label:'错误'}]}, {model:[{...good,html:'<b>'}]},
    {model:[good],on:[]}, {model:['legacy-string']},
  ];
  for(const options of invalid) {
    h.reply={ok:true,running:false,exit:0,options,event:{}};await h.ext.extensionTick();
    assert.equal(modelOptions(h),before);
    assert.equal(h.ext.inputs.get('model').value,'old-model');
  }
});

test('partial catalogues enforce each field byte limit and the merged 64-model bound',async()=>{
  const h=dynamicFixture();h.schema.fields[0].maxBytes=5;
  h.schema.fields.push({name:'other',type:'select',dynamic:true,label:'另一模型',options:[]});
  const full=Array.from({length:64},(_,i)=>({value:String(i),label:'模型 '+i}));
  h.reply={ok:true,running:false,exit:0,options:{model:full},event:{}};
  await h.ext.openExtension('partial.sh');await settle();
  const before=modelOptions(h);
  h.reply={ok:true,running:false,exit:0,event:{options:{model:[{value:'汉字',label:'超出字段限制'}]}}};
  await h.ext.extensionTick();assert.equal(modelOptions(h),before);
  h.reply={ok:true,running:false,exit:0,event:{options:{other:[{value:'new',label:'第65项'}]}}};
  await h.ext.extensionTick();assert.equal(h.ext.inputs.get('other').children.length,1);
  assert.equal(h.ext.inputs.get('other').value,'');
  h.reply={ok:true,running:false,exit:0,event:{options:{model:[],other:[{value:'new',label:'现在可添加'}]}}};
  await h.ext.extensionTick();
  assert.equal(h.ext.inputs.get('other').children.find(o=>o.value==='new').textContent,'现在可添加');
});

test('closing a model dialog clears its catalogue and late state cannot leak into another script',async()=>{
  const h=dynamicFixture();h.reply={ok:true,running:false,exit:0,options:{model:[{value:'account-a',label:'账户 A'}]},event:{}};
  await h.ext.openExtension('a.sh');await settle();
  const api=h.api;let resolve;
  h.api=()=>new Promise(r=>resolve=r);
  const pending=h.ext.extensionTick();h.ext.closeExtension();
  resolve({ok:true,running:false,exit:0,options:{model:[{value:'late-a',label:'晚到的 A'}]},event:{}});await pending;
  h.api=api;h.schema.fields[0].default='account-b';h.reply={ok:true,running:false,exit:0,options:{},event:{}};
  await h.ext.openExtension('b.sh');await settle();
  assert.equal(h.ext.inputs.get('model').value,'account-b');
  assert.ok(!modelOptions(h).some(o=>['account-a','late-a'].includes(o.value)));
});
function uninstallFixture() {
  const pending=[],logs=[],tasks=[];
  const ctx={Map,Date,Promise,pending,logs,tasks,window:{confirm:()=>true},consoleTarget:{textContent:'demo.sh'},
    wwLoad:()=>logs.push('wake-refresh'),appendLog:t=>logs.push(t),stopConsolePoll:()=>logs.push('stop-log'),refreshStorage:()=>logs.push('refresh'),
    poller:{after:f=>{tasks.push(f);return tasks.length;}},api:()=>new Promise(r=>pending.push(r))};
  vm.createContext(ctx);
  const code=html.slice(html.indexOf('  const uninstallJobs ='),html.indexOf('  /* ---------- init 开机'));
  vm.runInContext(code+'\nglobalThis.remove={uninstallScript,uninstallStep,jobs:uninstallJobs};',ctx);
  return ctx;
}
test('pending uninstall uses shared scheduler and suppresses duplicate clicks',async()=>{
  const h=uninstallFixture();h.remove.uninstallScript('demo.sh');h.remove.uninstallScript('demo.sh');
  assert.equal(h.pending.length,1);h.pending.shift()({ok:true,pending:true});await settle();
  assert.equal(h.tasks.length,1);assert.equal(h.remove.jobs.size,1);assert.ok(!h.logs.includes('refresh'));
  h.tasks.shift()();h.pending.shift()({ok:true});await settle();
  assert.equal(h.remove.jobs.size,0);assert.ok(h.logs.includes('refresh'));
  assert.ok(!h.logs.includes('stop-log')); // Keep following the other scripts.
});
test('uninstall stops at deadline and ignores an in-flight result after logout',async()=>{
  const h=uninstallFixture();h.remove.jobs.set('demo.sh',null);h.remove.uninstallStep('demo.sh',Date.now()-61000);
  h.pending.shift()({ok:true,pending:true});await settle();assert.equal(h.tasks.length,0);assert.equal(h.remove.jobs.size,0);
  h.remove.uninstallScript('demo.sh');h.remove.jobs.clear();h.pending.shift()({ok:true,pending:true});await settle();
  assert.equal(h.tasks.length,0);
});
