// Offline DOM regression for the actual status renderer; geometry is tested in a browser.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const html = fs.readFileSync(__dirname + '/index.html', 'utf8');

class Element {
  constructor(tag = 'div') { this.tagName = tag; this.children = []; this._text = ''; this.events = {}; }
  set textContent(value) { this._text = String(value); this.children = []; }
  get textContent() { return this._text + this.children.map(c => c.textContent).join(''); }
  set innerHTML(value) { this._text = String(value); this.children = []; }
  append(...children) { this.children.push(...children); }
  appendChild(child) { this.append(child); }
  addEventListener(name, callback) { this.events[name] = callback; }
}
function fixture() {
  const calls = [];
  const ctx = { appsCount: new Element(), appsList: new Element(), document: { createElement: tag => new Element(tag) },
    refreshAutostart() {}, openLogModal(arg) { calls.push(['log', arg.name]); },
    uninstallScript(name) { calls.push(['uninstall', name]); }, openExtension(name) { calls.push(['extension', name]); } };
  vm.createContext(ctx);
  const start = html.indexOf('  const formatBytes =');
  const end = html.indexOf('  /* ---------- Copyable codes', start);
  assert.ok(start >= 0 && end > start);
  vm.runInContext(html.slice(start, end) + '\nglobalThis.render = refreshAppsFromStatus;', ctx);
  return { ctx, calls };
}
function find(node, cls) {
  if ((node.className || '').split(' ').includes(cls)) return node;
  for (const child of node.children) { const result = find(child, cls); if (result) return result; }
}
for (const count of [0, 1, 4, 20]) {
  test(`status renderer retains ${count} rows and actions across script name widths`, () => {
    const { ctx, calls } = fixture();
    const names = ['grok.sh', '账户'.repeat(80) + '.sh', 'W'.repeat(180) + '.sh', '<img src=x onerror=alert(1)>.sh'];
    const apps = Array.from({ length: count }, (_, i) => ({ name: names[i % names.length] + i, installed: 1700000000,
      size: 10000, status: i % 2, root: 1, extension: 1, conflict: 1, audio_mode: 'phone-bridge', uninstallable: i === count - 1 }));
    ctx.render({ apps });
    assert.equal(ctx.appsCount.textContent, count ? `${count} 个脚本` : '');
    assert.equal(ctx.appsList.children.length, count);
    if (!count) assert.match(ctx.appsList.textContent, /暂无脚本/);
    apps.forEach((app, i) => {
      const row = ctx.appsList.children[i];
      assert.equal(find(row, 'app-name').textContent, app.name);
      const meta = find(row, 'app-meta');
      assert.doesNotMatch(meta.textContent, /打断方式/);
      assert.equal(meta.children.length, 3); // deployment, size, conflict
      const buttons = find(row, 'app-acts').children;
      assert.deepEqual(buttons.map(b => b.textContent), ['扩展', '日志', '卸载']);
      assert.equal(buttons[2].disabled, !app.uninstallable);
      buttons[0].events.click(); buttons[1].events.click();
      if (app.uninstallable) buttons[2].events.click();
    });
    assert.equal(calls.length, count * 2 + (count ? 1 : 0));
    if (count) assert.deepEqual(calls.at(-1), ['uninstall', apps.at(-1).name]);
    ctx.render({ apps: [] });
    assert.equal(ctx.appsList.children.length, 0);
    assert.equal(ctx.appsCount.textContent, '');
  });
}
