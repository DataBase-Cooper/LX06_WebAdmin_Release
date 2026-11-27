const test=require('node:test');
const assert=require('node:assert/strict');
const vm=require('node:vm');
const fs=require('node:fs');
class Element {
  constructor(tag='div'){this.tag=tag;this.children=[];this.style={};this.events={};this.value='';}
  set textContent(value){this.children=[{textContent:value}];}
  get textContent(){return this.children.map(c=>c.textContent||'').join('');}
  append(...items){this.children.push(...items);}
  replaceChildren(...items){this.children=items;}
  setAttribute(){}
  addEventListener(name,fn){this.events[name]=fn;}
  focus(){this.focused=true;}
  select(){this.selected=true;}
  remove(){this.removed=true;}
}
function fixture(secure=false){
  const html=fs.readFileSync(__dirname+'/index.html','utf8');
  const code=html.slice(html.indexOf('  const copyCode ='),html.indexOf('  /* ---------- End copyable codes'));
  const copied=[],fields=[],active=new Element('button');
  const ctx={isSecureContext:secure,navigator:{clipboard:{async writeText(v){copied.push(v);}}},
    window:{getSelection:()=>({rangeCount:0,removeAllRanges(){},addRange(){}})},
    document:{activeElement:active,body:new Element('body'),
      createElement(tag){const e=new Element(tag);if(tag==='textarea')fields.push(e);return e;},
      createTextNode:text=>({textContent:text,nodeType:3}),
      execCommand(command){assert.equal(command,'copy');const f=fields.at(-1);assert.equal(f.selected,true);copied.push(f.value);return true;}}};
  vm.createContext(ctx);vm.runInContext(code+'\nglobalThis.copy={copyCode,codeCopyButton,logText,setLogText};',ctx);
  return {ctx,copied,fields,active,...ctx.copy};
}
test('labelled codes get inline buttons; plain numbers, secrets and script HTML stay inert',()=>{
  const h=fixture(),e=new Element();
  const text='绑定码 001234 请填写\n验证码：ABCD-EFGH\nPID 123456\naccess_token=123456\n验证码已过期\n<img src=x onerror=alert(1)>';
  h.setLogText(e,text);
  assert.equal(e.children.filter(c=>c.tag==='button').length,2);
  assert.equal(h.logText(e),text);
  assert.ok(e.children.every(c=>c.nodeType===3||c.tag==='button'));
  h.setLogText(e,h.logText(e)+'\n新日志');assert.equal(e.children.filter(c=>c.tag==='button').length,2);
  assert.doesNotMatch(h.logText(e),/复制/);
  h.setLogText(e,'');assert.equal(e.textContent,'');assert.equal(h.logText(e),'');
});
test('HTTP click copies exact value including leading zero and restores focus',async()=>{
  const h=fixture(),b=h.codeCopyButton('001234');assert.deepEqual(h.copied,[]);
  await b.events.click();assert.deepEqual(h.copied,['001234']);
  assert.equal(b.textContent,'已复制');assert.equal(h.fields[0].removed,true);assert.equal(h.active.focused,true);
});
test('copy requires script opt-in, including mixed-script log batches',()=>{
  const h=fixture(),e=new Element();
  const text='[xz_watch.sh] 绑定码 001234\n[other.sh] 验证码 987654';
  h.setLogText(e,text,false);
  assert.equal(e.children.filter(c=>c.tag==='button').length,0);
  const html=fs.readFileSync(__dirname+'/index.html','utf8');
  const start=html.indexOf('  const consoleCopyScripts =');
  const end=html.indexOf('  const appendLog',start);
  vm.runInContext(html.slice(start,end)+'\nconsoleCopyScripts.set("xz_watch.sh",true);globalThis.allowed=consoleCodeAllowed;',h.ctx);
  h.setLogText(e,text,h.ctx.allowed);
  assert.equal(e.children.filter(c=>c.tag==='button').length,1);
  assert.equal(h.logText(e),text);
});
test('HTTPS uses Clipboard API; rejected clipboard falls back without changing value',async()=>{
  const h=fixture(true),b=h.codeCopyButton('ABCD-EFGH');await b.events.click();
  assert.deepEqual(h.copied,['ABCD-EFGH']);assert.equal(h.fields.length,0);
  h.ctx.navigator.clipboard.writeText=async()=>{throw new Error('denied');};
  await b.events.click();assert.deepEqual(h.copied,['ABCD-EFGH','ABCD-EFGH']);
});
test('failed copy never claims success; code remains selectable; rendering is bounded',async()=>{
  const h=fixture(),e=new Element();h.ctx.document.execCommand=()=>false;
  const b=h.codeCopyButton('123456');await b.events.click();assert.equal(b.textContent,'请手动复制');
  h.setLogText(e,('绑定码 123456\n').repeat(100));
  assert.equal(e.children.filter(c=>c.tag==='button').length,64);
  assert.equal(h.logText(e).length,('绑定码 123456\n').repeat(100).length);
});
