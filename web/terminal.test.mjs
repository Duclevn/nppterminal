import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';

const source = await readFile(new URL('./terminal.js', import.meta.url), 'utf8');
function page() {
  const sent = [], writes = [], callbacks = [];
  const status = { textContent: '' };
  const host = { clientWidth: 800, clientHeight: 400 };
  const rootStyle = { values: {}, setProperty(name, value) { this.values[name] = value; } };
  let listener, input, binary, terminal, osc, keyHandler, selection = '', pasted = [], focusCount = 0;
  class FakeTerminal {
    constructor(options) { this.options = options; this.cols = 80; this.rows = 24; terminal = this; this.parser = { registerOscHandler: (id, callback) => { osc = { id, callback }; } }; }
    loadAddon() {} open() {} reset() {} focus() { focusCount += 1; } clear() {}
    onData(callback) { input = callback; }
    onBinary(callback) { binary = callback; }
    attachCustomKeyEventHandler(callback) { keyHandler = callback; }
    getSelection() { return selection; }
    paste(data) { pasted.push(data); }
    write(bytes, callback) { writes.push([...bytes]); callbacks.push(callback); }
  }
  vm.runInNewContext(source, {
    window: { chrome: { webview: { postMessage: message => sent.push(message), addEventListener: (_, callback) => { listener = callback; } } } },
    document: { documentElement: { style: rootStyle }, getElementById: id => id === 'status' ? status : host },
    Terminal: FakeTerminal, FitAddon: { FitAddon: class { fit() {} } },
    ResizeObserver: class { observe() {} }, TextEncoder, Uint8Array, atob
  });
  return {
    sent, writes, callbacks, status, terminal, osc, rootStyle,
    receive: message => listener({ data: message }), input: data => input(data), binary: data => binary(data),
    key: event => keyHandler(event), setSelection: value => { selection = value; },
    pasted, focusCount: () => focusCount
  };
}

test('input remains disabled until a valid generation is running', () => {
  const p = page();
  p.input('a'); assert.equal(p.sent.length, 0);
  p.receive({ type: 'init', generation: -1 }); assert.equal(p.sent.length, 0);
  p.receive({ type: 'init', generation: 7 });
  assert.equal(p.sent.at(-1).type, 'ready');
  assert.equal(p.sent.at(-1).cols, 80);
  p.input('a'); assert.equal(p.sent.at(-1).type, 'ready');
  p.receive({ type: 'state', generation: 6, state: 'Running' });
  assert.equal(p.terminal.options.disableStdin, true);
  p.receive({ type: 'state', generation: 7, state: 'Running' });
  p.input('Tiếng Việt');
  assert.equal(p.sent.at(-1).data, 'Tiếng Việt');
  assert.equal(p.sent.at(-1).generation, 7);
});

test('output preserves split UTF-8 bytes and is acknowledged only after rendering', () => {
  const p = page(); p.receive({ type: 'init', generation: 1 });
  const bytes = Buffer.from('日本語🙂');
  const parts = [bytes.subarray(0, 2), bytes.subarray(2)];
  for (const [id, data] of parts.entries()) p.receive({ type: 'output', generation: 1, id, data: data.toString('base64') });
  assert.equal(p.sent.filter(x => x.type === 'ack').length, 0);
  assert.deepEqual(p.writes.flat(), [...bytes]);
  p.callbacks.forEach(callback => callback());
  assert.deepEqual(p.sent.filter(x => x.type === 'ack').map(x => x.id), [0, 1]);
});

test('late render acknowledgments cannot reach a replacement session', () => {
  const p = page(); p.receive({ type: 'init', generation: 1 });
  p.receive({ type: 'output', generation: 1, id: 1, data: 'YQ==' });
  p.receive({ type: 'init', generation: 2 });
  p.callbacks[0]();
  assert.equal(p.sent.filter(x => x.type === 'ack').length, 0);
  p.receive({ type: 'output', generation: 1, id: 2, data: 'Yg==' });
  assert.equal(p.writes.length, 1);
});

test('oversized and malformed output is rejected before reaching xterm', () => {
  const p = page(); p.receive({ type: 'init', generation: 1 });
  for (const message of [null, [], { type: 'output', generation: 1, id: 1, data: '%%%' }, { type: 'output', generation: 1, id: 1, data: Buffer.alloc(32769).toString('base64') }]) p.receive(message);
  assert.equal(p.writes.length, 0);
});

test('oversized paste is visibly rejected and OSC52 writes are consumed', () => {
  const p = page(); p.receive({ type: 'init', generation: 1 });
  p.receive({ type: 'state', generation: 1, state: 'Running' });
  const before = p.sent.length;
  p.input('a'.repeat(65537));
  assert.equal(p.sent.length, before);
  assert.match(p.status.textContent, /Paste is too large/);
  assert.equal(p.osc.id, 52); assert.equal(p.osc.callback('anything'), true);
});

test('input is serialized until the native writer acknowledges complete writes', () => {
  const p = page(); p.receive({ type: 'init', generation: 1 });
  p.receive({ type: 'state', generation: 1, state: 'Running' });
  p.input('first'); p.input('second'); p.input('third');
  let messages = p.sent.filter(x => x.type === 'input');
  assert.equal(messages.length, 1); assert.equal(messages[0].data, 'first');
  p.receive({ type: 'inputAck', generation: 1, id: 999 });
  p.receive({ type: 'inputAck', generation: 2, id: 1 });
  assert.equal(p.sent.filter(x => x.type === 'input').length, 1);
  p.receive({ type: 'inputAck', generation: 1, id: messages[0].id });
  messages = p.sent.filter(x => x.type === 'input');
  assert.equal(messages.length, 2); assert.equal(messages[1].data, 'secondthird');
  assert.equal(messages[1].id, 2);
});

test('bounded input backlog pauses stdin and resumes after writer progress', () => {
  const p = page(); p.receive({ type: 'init', generation: 1 });
  p.receive({ type: 'state', generation: 1, state: 'Running' });
  p.input('first'); p.input('a'.repeat(49152));
  assert.equal(p.terminal.options.disableStdin, true);
  assert.match(p.status.textContent, /Input paused/);
  p.input('b'.repeat(16385));
  assert.match(p.status.textContent, /queue is full/);
  p.receive({ type: 'inputAck', generation: 1, id: 1 });
  assert.equal(p.terminal.options.disableStdin, false);
  assert.equal(p.sent.filter(x => x.type === 'input').at(-1).data.length, 49152);
});

test('binary input preserves byte strings and replacement discards the old input backlog', () => {
  const p = page(); p.receive({ type: 'init', generation: 1 });
  p.receive({ type: 'state', generation: 1, state: 'Running' });
  p.binary('\u0000\u00ff'); p.input('old pending');
  assert.equal(p.sent.at(-1).type, 'binary');
  assert.equal(p.sent.at(-1).data, '\u0000\u00ff');
  p.receive({ type: 'init', generation: 2 });
  p.receive({ type: 'state', generation: 2, state: 'Running' });
  p.receive({ type: 'inputAck', generation: 1, id: 1 });
  p.input('new');
  assert.equal(p.sent.at(-1).data, 'new'); assert.equal(p.sent.at(-1).generation, 2);
});

test('native settings and theme are validated and applied live', () => {
  const p = page();
  p.receive({ type: 'init', generation: 3, settings: { fontFamily: 'Cascadia Mono', fontSize: 17, scrollback: 9000 }, theme: { background: '#101010', foreground: '#f0f0f0' } });
  assert.equal(p.sent.at(-1).type, 'ready');
  assert.equal(p.terminal.options.fontFamily, 'Cascadia Mono');
  assert.equal(p.terminal.options.fontSize, 17);
  assert.equal(p.terminal.options.scrollback, 9000);
  assert.equal(p.terminal.options.theme.background, '#101010');
  assert.equal(p.rootStyle.values['--terminal-background'], '#101010');
  p.receive({ type: 'settings', generation: 3, settings: { fontSize: 49 } });
  assert.equal(p.terminal.options.fontSize, 17);
  p.receive({ type: 'theme', generation: 3, theme: { background: 'url(https://remote.invalid)' } });
  assert.equal(p.terminal.options.theme.background, '#101010');
});

test('clipboard uses explicit native messages and leaves Ctrl+C as terminal input', () => {
  const p = page();
  p.receive({ type: 'init', generation: 4 });
  p.receive({ type: 'state', generation: 4, state: 'Running' });
  p.setSelection('Tiếng Việt 🙂');
  assert.equal(p.key({ type: 'keydown', key: 'C', ctrlKey: true, shiftKey: true, altKey: false }), false);
  assert.equal(p.sent.at(-1).type, 'clipboardCopy');
  assert.equal(p.sent.at(-1).data, 'Tiếng Việt 🙂');
  assert.equal(p.key({ type: 'keydown', key: 'C', ctrlKey: true, shiftKey: false, altKey: false }), true);
  assert.equal(p.sent.filter(x => x.type === 'clipboardCopy').length, 1);
  assert.equal(p.key({ type: 'keydown', key: 'V', ctrlKey: true, shiftKey: true, altKey: false }), false);
  assert.equal(p.sent.at(-1).type, 'clipboardRead');
  p.receive({ type: 'clipboard', generation: 4, data: 'paste text' });
  assert.deepEqual(p.pasted, ['paste text']);
  p.receive({ type: 'clipboard', generation: 4, data: 'x'.repeat(65537) });
  assert.deepEqual(p.pasted, ['paste text']);
});

test('focus message focuses the terminal and stale native settings are ignored', () => {
  const p = page(); p.receive({ type: 'init', generation: 5 });
  const before = p.focusCount();
  p.receive({ type: 'focus', generation: 5 });
  assert.equal(p.focusCount(), before + 1);
  p.receive({ type: 'settings', generation: 4, settings: { fontSize: 22 } });
  assert.equal(p.terminal.options.fontSize, 13);
});
