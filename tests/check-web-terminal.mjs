import { readFile } from 'node:fs/promises';
const source = (await readFile(new URL('../web/terminal.js', import.meta.url), 'utf8')).replace(/^import[^\n]*\n/gm, '').replace(/^export /gm, '');
const mount = new Function(source + '\nreturn mountTerminal;')();
const assert = (value, message) => { if (!value) throw new Error(message); };
class Target {
  constructor() { this.events = new Map(); }
  addEventListener(type, fn) { this.events.set(type, [...(this.events.get(type) || []), fn]); }
  fire(type, event = {}) { for (const fn of this.events.get(type) || []) fn(event); }
}
function boot(options = {}) {
  const nodes = new Map(), raf = new Map(); let serial = 0, terminal, socket, observer;
  const document = new Target();
  Object.assign(document, { documentElement: { dataset: {} }, hasFocus: () => options.focus !== false, querySelector: () => ({ getBoundingClientRect: () => ({ height: 40 }) }), getElementById(id) { if (!nodes.has(id)) nodes.set(id, new Target()); return nodes.get(id); } });
  class Terminal {
    constructor(options) { this.options = options; this.rows = 24; this.cols = 80; this.writes = []; terminal = this; }
    loadAddon(addon) { this.fit = addon; }
    open() { this.element = { querySelector: () => ({ getBoundingClientRect: () => ({ height: this.rows * 18 }) }) }; } focus() { this.focused = true; }
    onData(fn) { this.data = fn; } onBinary(fn) { this.binary = fn; }
    write(bytes, done) { this.writes.push({ bytes, done }); }
  }
  class Socket extends Target {
    static OPEN = 1;
    constructor(url) { super(); this.url = url; this.readyState = 0; this.sent = []; socket = this; }
    send(data) { if (this.readyState !== 1) throw Error('closed'); this.sent.push(data); }
    close() { if (this.readyState === 3) return; this.readyState = 3; this.fire('close'); }
  }
  const window = new Target();
  Object.assign(window, { URL, WebSocket: Socket, location: { href: 'https://localhost/chat/terminal', protocol: 'https:', origin: 'https://localhost' },
    getComputedStyle: () => ({ getPropertyValue: () => '' }), close() { this.closed = true; },
    requestAnimationFrame: fn => { const id = ++serial; raf.set(id, fn); return id; }, cancelAnimationFrame: id => raf.delete(id),
    ResizeObserver: class { constructor(fn) { this.fn = fn; observer = this; } observe() {} disconnect() { this.disconnected = true; } } });
  window.parent = options.embedded ? { messages: [], postMessage(data, origin) { this.messages.push({ data, origin }); } } : window;
  mount({ document, window, Terminal, FitAddon: class { fit() {} } });
  const controls = () => socket.sent.filter(data => typeof data === 'string').map(data => new URLSearchParams(data));
  return { terminal, socket, window, document, nodes, observer, controls,
    open() { socket.readyState = 1; socket.fire('open'); },
    event(type, fields = {}) { socket.fire('message', { data: JSON.stringify({ type, ...fields }) }); },
    binary(bytes) { socket.fire('message', { data: Uint8Array.from(bytes).buffer }); },
    frames() { return socket.sent.filter(data => typeof data !== 'string'); },
    resize() { observer.fn(); for (const [id, fn] of [...raf]) { raf.delete(id); fn(); } } };
}
function check(name, fn, options) { const ui = boot(options); fn(ui); console.log('PASS ' + name); }
check('terminal connects to the mounted secure endpoint with measured geometry', ui => {
  assert(ui.socket.url.href === 'wss://localhost/chat/terminal/events', 'wrong endpoint/mount');
  assert(ui.terminal.options.disableStdin, 'input enabled before ready'); ui.open();
  assert(ui.controls()[0].get('action') === 'open' && ui.controls()[0].get('columns') === '80', 'open geometry missing');
  ui.event('ready'); assert(ui.terminal.focused && !ui.terminal.options.disableStdin, 'ready did not activate terminal');
});
check('PTY output is acknowledged only after the renderer consumes bytes', ui => {
  ui.open(); ui.event('ready'); ui.binary([0, 255, 0xe2, 0x82, 0xac]);
  assert(!ui.controls().some(frame => frame.get('action') === 'ack'), 'receipt acknowledged before rendering');
  assert(ui.terminal.writes[0].bytes[1] === 255, 'PTY bytes were decoded as text');
  ui.terminal.writes[0].done();
  assert(ui.controls().at(-1).get('bytes') === '5', 'wrong byte count acknowledged');
});
check('input is UTF-8, bounded to chunks and gated by server credit', ui => {
  ui.open(); ui.event('ready'); ui.terminal.data('€' + 'x'.repeat(5000));
  assert(!ui.frames().length, 'input sent before credit'); ui.event('input_credit');
  assert(ui.frames().length === 1 && ui.frames()[0].length === 4096, 'input chunk bound violated');
  assert(ui.frames()[0][0] === 0xe2, 'input lost UTF-8'); ui.event('input_credit');
  assert(ui.frames().length === 2 && ui.frames()[1].length === 907, 'partial input was dropped');
});
check('binary terminal input preserves byte values', ui => {
  ui.open(); ui.event('ready'); ui.event('input_credit'); ui.terminal.binary('\x00\x80\xff');
  assert([...ui.frames()[0]].join(',') === '0,128,255', 'binary input was encoded as UTF-8');
});
check('resize reports current geometry without restarting the shell', ui => {
  ui.open(); ui.event('ready'); ui.terminal.cols = 113; ui.terminal.rows = 41; ui.resize();
  assert(ui.controls().at(-1).get('action') === 'resize' && ui.controls().at(-1).get('rows') === '41', 'resize was lost');
  assert(ui.controls().filter(frame => frame.get('action') === 'open').length === 1, 'resize reopened terminal');
});
check('pending input overflow explicitly closes the session', ui => {
  ui.open(); ui.event('ready'); ui.terminal.data('x'.repeat(65537));
  assert(ui.socket.readyState === 3 && ui.nodes.get('terminal-status').textContent.includes('64 KiB'), 'overflow was hidden or unbounded');
});
check('output window violations close rather than retain unbounded work', ui => {
  ui.open(); ui.event('ready');
  for (let i = 0; i < 17; ++i) ui.binary(new Uint8Array(4096));
  assert(ui.socket.readyState === 3 && ui.terminal.writes.length === 16, 'output exceeded window');
});
check('shell exit preserves final pending output and does not acknowledge a closed session', ui => {
  ui.open(); ui.event('ready'); ui.binary([65]); ui.event('exit', { status: '7' }); ui.socket.close();
  ui.terminal.writes[0].done();
  assert(ui.nodes.get('terminal-status').textContent === 'Shell exited (7).', 'exit status lost final output');
  assert(!ui.controls().some(frame => frame.get('action') === 'ack'), 'ack was sent after exit');
});
check('connection loss disables typing and never starts another shell automatically', ui => {
  ui.open(); ui.event('ready'); ui.terminal.data('pending'); ui.socket.close(); ui.terminal.data('later');
  assert(ui.terminal.options.disableStdin && ui.observer.disconnected && ui.controls().filter(frame => frame.get('action') === 'open').length === 1, 'disconnect retained input or restarted shell');
});
check('server errors keep their actionable reason', ui => {
  ui.open(); ui.event('error', { text: 'Shell launch failed' });
  assert(ui.socket.readyState === 3 && ui.nodes.get('terminal-status').textContent === 'Shell launch failed', 'server failure reason was overwritten');
});
check('page close cancels the owned session', ui => {
  ui.open(); ui.event('ready'); ui.window.fire('pagehide');
  assert(ui.socket.readyState === 3 && ui.terminal.options.disableStdin, 'page close retained session');
});
check('embedded shell exit closes the panel only after final output is consumed', ui => {
  ui.open(); ui.event('ready'); ui.binary([65]); ui.event('exit', { status: '0' }); ui.socket.close();
  const closed = () => ui.window.parent.messages.filter(message => message.data.type === 'softline-terminal-closed');
  assert(!closed().length, 'panel closed before final output was rendered');
  ui.terminal.writes[0].done();
  assert(closed().length === 1 && closed()[0].origin === 'https://localhost', 'exit did not notify its parent exactly once');
  ui.nodes.get('terminal-close').fire('click');
  assert(closed().length === 1 && !ui.window.closed, 'embedded close tried to close the browser tab or notified twice');
}, { embedded: true });
check('embedded close button terminates the transport and requests panel removal', ui => {
  ui.open(); ui.event('ready'); ui.nodes.get('terminal-close').fire('click');
  assert(ui.socket.readyState === 3 && ui.terminal.options.disableStdin && ui.window.parent.messages.some(message => message.data.type === 'softline-terminal-closed'), 'close left the embedded shell alive');
}, { embedded: true });
check('embedded measurements describe actual rendered rows and terminal chrome', ui => {
  ui.open(); ui.event('ready'); ui.terminal.rows = 31; ui.resize();
  const message = ui.window.parent.messages.filter(message => message.data.type === 'softline-terminal-size').at(-1);
  assert(message.data.rowHeight === 18 && message.data.chromeHeight === 40 && message.origin === 'https://localhost', 'measurements lost row geometry or origin');
}, { embedded: true });
check('embedded theme and stop messages require the owning same-origin parent', ui => {
  ui.open(); ui.event('ready');
  const message = (data, source = ui.window.parent, origin = 'https://localhost') => ui.window.fire('message', { data, source, origin });
  message({ type: 'softline-terminal-stop' }, {});
  message({ type: 'softline-terminal-stop' }, ui.window.parent, 'https://other.example');
  assert(ui.socket.readyState === 1, 'untrusted message terminated the shell');
  message({ type: 'softline-terminal-theme', theme: 'monogreen' });
  assert(ui.document.documentElement.dataset.theme === 'monogreen', 'parent theme did not apply');
  message({ type: 'softline-terminal-stop' });
  assert(ui.socket.readyState === 3 && ui.terminal.options.disableStdin, 'parent close left the shell alive');
}, { embedded: true });
check('embedded terminal shortcut closes without forwarding input or closing the tab', ui => {
  ui.open(); ui.event('ready');
  const event = { ctrlKey: true, code: 'Backquote', preventDefault() { this.prevented = true; }, stopImmediatePropagation() { this.stopped = true; } };
  ui.document.fire('keydown', event);
  assert(event.prevented && event.stopped && ui.socket.readyState === 3 && !ui.window.closed && !ui.frames().length, 'terminal shortcut was forwarded or closed the browser');
}, { embedded: true });
check('delayed embedded shell readiness does not steal focus back from chat', ui => {
  ui.open(); ui.event('ready');
  assert(!ui.terminal.focused && !ui.terminal.options.disableStdin, 'late readiness displaced the current chat control');
}, { embedded: true, focus: false });
