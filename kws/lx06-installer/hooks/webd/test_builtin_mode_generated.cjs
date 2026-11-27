// Generated synthetic account schemas through the production extension renderer.
const test=require('node:test'), assert=require('node:assert/strict');
const fs=require('node:fs'), path=require('node:path'), {execFileSync}=require('node:child_process');
const harness=fs.readFileSync(path.join(__dirname,'test_web_extensions.cjs'),'utf8');
const fixture=new Function('require','__dirname',harness.slice(0,harness.indexOf('const settle='))+';return fixture;')(require,__dirname);
const settle=()=>new Promise(setImmediate);
const root=path.resolve(__dirname,'../../runtime/model-adapter');
const generator="import sys,tempfile;from pathlib import Path;sys.path.insert(0,sys.argv[1]);import build_accounts;d=tempfile.TemporaryDirectory();p=Path(d.name);build_accounts.build(p);sys.stdout.buffer.write((p/(sys.argv[2]+'_account.sh')).read_bytes())";
for(const provider of ['chatgpt','grok']) {
  const script=execFileSync('python',['-c',generator,root,provider],{encoding:'utf8'});
  const schema=JSON.parse(script.split('# LX06-EXTENSION-BEGIN\n# ',2)[1].split('\n',1)[0]);
  for(const mode of [undefined,'','0','1']) test(`${provider}: generated mode ${mode===undefined?'absent':mode===''?'empty':mode} remains unknown during save and readback`,async()=>{
    const h=fixture(); Object.assign(h.schema,JSON.parse(JSON.stringify(schema)));
    const save=h.schema.actions.find(a=>a.id==='save');
    assert.ok(h.schema.fields.some(f=>f.name==='MODEL_BARGE_ANY'));
    assert.ok(!save.fields.includes('MODEL_BARGE_ANY'));
    const stored={MODEL_GREETING:'synthetic old'};if(mode!==undefined)stored.MODEL_BARGE_ANY=mode;
    let unblock;
    h.api=async(url,body)=>{
      h.urls.push({url,body});
      if(body && body.action==='save') return new Promise(resolve=>{unblock=()=>{Object.assign(stored,body.values);resolve({ok:true});};});
      if(body) return {ok:true};
      if(url.includes('/state?'))return {ok:true,running:false,exit:0,values:{...stored},event:{step:'configure'}};
      return {ok:true,schema:h.schema};
    };
    await h.ext.openExtension(provider+'_account.sh');await settle();await settle();
    const field=h.ext.inputs.get('MODEL_BARGE_ANY');
    assert.equal(field.extensionLabel.textContent,'打断方式：未验证');
    assert.equal(field.extensionSegments,undefined);
    const greeting=h.ext.inputs.get('MODEL_GREETING');greeting.value='synthetic new';greeting.events.input();
    const pending=h.ext.extensionExecute(save);await settle();assert.equal(typeof unblock,'function');
    const count=h.urls.filter(x=>x.body?.action==='save').length;
    await h.ext.extensionExecute(save);assert.equal(h.urls.filter(x=>x.body?.action==='save').length,count);
    assert.ok(!Object.hasOwn(h.urls.filter(x=>x.body?.action==='save').at(-1).body.values,'MODEL_BARGE_ANY'));
    unblock();await pending;await settle();await h.ext.extensionTick();
    assert.equal(stored.MODEL_GREETING,'synthetic new');assert.equal(stored.MODEL_BARGE_ANY,mode);
    assert.equal(Object.hasOwn(stored,'MODEL_BARGE_ANY'),mode!==undefined);
    assert.equal(field.extensionLabel.textContent,'打断方式：未验证');
  });
}
