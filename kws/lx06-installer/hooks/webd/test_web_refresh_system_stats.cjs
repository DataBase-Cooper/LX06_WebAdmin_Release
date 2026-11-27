const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

// Execute the production polling code with controllable network and timers.
const harness = () => require('./web_poll_harness.cjs')({systemOnly:true});

test('slow system-stats polling shares one request, times out and recovers', async () => {
  const h = harness();
  h.poll.startSystemStats();
  await h.advance(9000);
  assert.equal(h.requests.length, 1, 'three timer ticks must not overlap the pending request');
  await h.advance(6000);
  assert.equal(h.requests[0].options.signal.aborted, true, 'stalled request is aborted after 15s');
  await h.advance(5000);
  assert.equal(h.requests.length, 2, 'next interval retries after the timeout');
  h.requests[1].resolve({ status: 200, ok: true, json: async () => ({ ok: true, cpu: 42, memory: 25 }) });
  await h.advance(0);
  assert.equal(h.node('cpuValue').textContent, '42%');
  assert.equal(h.node('memoryValue').textContent, '25%');
  h.poll.stopSystemStats();
});

test('successful polls repeat and HTTP 401 stops polling', async () => {
  const h = harness();
  h.poll.startSystemStats();
  h.requests[0].resolve({ status: 200, ok: true, json: async () => ({ ok: true, cpu: 12, memory: 34 }) });
  await h.advance(0);
  await h.advance(5000);
  assert.equal(h.node('cpuValue').textContent, '12%');
  assert.equal(h.requests.length, 2);
  h.requests[1].resolve({ status: 401 });
  await h.advance(9000);
  assert.equal(h.logins, 1);
  assert.equal(h.requests.length, 2, 'unauthorized response stops further polling');
});

test('logout aborts old requests; queued old 401 cannot revoke a new session', async () => {
  const h = harness();
  h.poll.startSystemStats();
  h.requests[0].resolve({ status: 401 }); // Response queued, callback not yet executed.
  h.poll.stopSystemStats();
  assert.equal(h.requests[0].options.signal.aborted, true);
  h.poll.startSystemStats();
  assert.equal(h.requests.length, 2, 'new login starts its own request');
  await h.advance(0);
  assert.equal(h.logins, 0);
  await h.advance(5000);
  assert.equal(h.requests.length, 2, 'old finally must not clear the new in-flight request');
  h.poll.stopSystemStats();
});

test('late body from an old session cannot repaint the new session', async () => {
  const h = harness();
  let finishOldBody;
  h.poll.startSystemStats();
  h.requests[0].resolve({ status: 200, ok: true, json: () => new Promise(r => { finishOldBody = r; }) });
  await h.advance(0);
  h.poll.stopSystemStats();
  h.poll.startSystemStats();
  h.requests[1].resolve({ status: 200, ok: true, json: async () => ({ ok: true, cpu: 20, memory: 30 }) });
  await h.advance(0);
  finishOldBody({ ok: true, cpu: 77, memory: 90 });
  await h.advance(0);
  assert.equal(h.node('cpuValue').textContent, '20%');
  h.poll.stopSystemStats();
});

test('network failure, invalid payload and HTTP errors clear the entire meter', async () => {
  const h = harness();
  h.poll.startSystemStats();
  h.requests[0].resolve({ status: 200, ok: true, json: async () => ({ ok: true, cpu: 77, memory: 90 }) });
  await h.advance(0);
  assert.equal(h.node('cpuUsedMeter').attrs.width, '231');
  await h.advance(5000);
  h.requests[1].reject(new Error('network'));
  await h.advance(0);
  assert.equal(h.node('cpuValue').textContent, '--%');
  assert.equal(h.node('cpuUsedMeter').attrs.width, '0');
  assert.equal(h.node('cpuMeterTitle').textContent, 'CPU 已用 --%，剩余 --%');
  await h.advance(5000);
  h.requests[2].resolve({ status: 500, ok: false, json: async () => ({ ok: true, cpu: 88, memory: 20 }) });
  await h.advance(0);
  assert.equal(h.node('cpuValue').textContent, '--%');
  await h.advance(5000);
  h.requests[3].resolve({ status: 200, ok: true, json: async () => ({ ok: false }) });
  await h.advance(0);
  assert.equal(h.node('cpuValue').textContent, '--%');
  h.poll.stopSystemStats();
});

test('entire embedded script parses', () => {
  const html = fs.readFileSync(path.join(__dirname, 'index.html'), 'utf8');
  const script = html.match(/<script>([\s\S]*?)<\/script>/);
  assert.ok(script);
  new vm.Script(script[1]);
});
