// Offline RED: executes the shipped /api/status apps renderer, not a copy.
// No trusted bound-mode status is currently published; no hypothetical field is
// treated as authenticated here. These fixtures do not run scripts or devices.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const html = fs.readFileSync(__dirname + '/index.html', 'utf8');

class Node {
  constructor(tag='div') { this.tagName=tag; this.children=[]; this._text=''; }
  set textContent(value) { this._text=String(value); this.children=[]; }
  get textContent() { return this._text + this.children.map(child=>child.textContent).join(''); }
  set innerHTML(value) { if (value !== '') throw Error('unexpected markup'); this.children=[]; this._text=''; }
  append(...children) { this.children.push(...children); }
  appendChild(child) { this.append(child); }
  addEventListener() {}
}
function renderer() {
  const ctx = {appsCount:new Node(), appsList:new Node(), document:{createElement:tag=>new Node(tag)},
    refreshAutostart(){}, openLogModal(){}, uninstallScript(){}, openExtension(){}};
  vm.createContext(ctx);
  const start=html.indexOf('  const formatBytes =');
  const end=html.indexOf('  /* ---------- Copyable codes',start);
  assert.ok(start>=0 && end>start, 'production renderer must be available');
  vm.runInContext(html.slice(start,end)+'\nglobalThis.render=refreshAppsFromStatus;',ctx);
  return ctx;
}

for (const extra of [{},{MODEL_BARGE_ANY:'0'},{MODEL_BARGE_ANY:'1'},
  {interruption:'voice'},{interruption:'keyword'}]) {
  test(`unbound account is unverified: ${JSON.stringify(extra)}`,()=>{
    const h=renderer();
    h.render({apps:[{name:'grok_account.sh',status:0,installed:0,size:0,...extra}]});
    const row=h.appsList.children[0];
    assert.match(row.textContent,/未验证/,'missing unverified status');
    assert.doesNotMatch(row.textContent,/打断方式[：:]\s*(人声|打断词)/);
  });
}
