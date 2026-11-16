// Logical client tests: execute the shipped module with DOM/transport doubles.
// Actual browser layout and selection integration remain in web-browser.html.
export async function runWebCompletionTests(source) {
  const checks = [];
  function assert(value, message) { if (!value) throw new Error(message); }
  class Target {
    constructor() { this.listeners = new Map(); }
    addEventListener(name, fn) { const list = this.listeners.get(name) || []; list.push(fn); this.listeners.set(name, list); }
    fire(name, fields = {}) {
      const event = { key: '', altKey: false, shiftKey: false, ctrlKey: false, isComposing: false, defaultPrevented: false, preventDefault() { this.defaultPrevented = true; }, ...fields };
      for (const fn of this.listeners.get(name) || []) fn(event);
      return event;
    }
  }
  class Element extends Target {
    constructor() {
      super(); this.value = ''; this.selectionStart = this.selectionEnd = 0;
      this.hidden = false; this.children = []; this.style = {}; this.dataset = {}; this.options = [];
      this.scrollHeight = this.clientHeight = 36; this.scrollTop = 0;
      this.classList = { add() {}, remove() {} };
    }
    setAttribute() {}
    append(...values) { for (const value of values) { if (value && typeof value === 'object') value.parent = this; this.children.push(value); } }
    replaceChildren(...values) { this.children = values; }
    get childElementCount() { return this.children.length; }
    focus() {}
    scrollIntoView() {}
    remove() { if (this.parent) this.parent.children = this.parent.children.filter(child => child !== this); }
    click() { this.fire('click'); }
    setSelectionRange(start, end) { this.selectionStart = start; this.selectionEnd = end; }
    setRangeText(text, start, end, mode) {
      const oldStart = this.selectionStart, oldEnd = this.selectionEnd, delta = text.length - end + start;
      this.value = this.value.slice(0, start) + text + this.value.slice(end);
      if (mode === 'end') this.selectionStart = this.selectionEnd = start + text.length;
      else {
        this.selectionStart = oldStart > end ? oldStart + delta : oldStart > start ? start : oldStart;
        this.selectionEnd = oldEnd > end ? oldEnd + delta : oldEnd > start ? start + text.length : oldEnd;
      }
    }
  }
  class Encoder { encode(text) { return Uint8Array.from(unescape(encodeURIComponent(text)), c => c.charCodeAt(0)); } }
  class Decoder { decode(bytes = []) { return decodeURIComponent(escape(Array.from(bytes, n => String.fromCharCode(n)).join(''))); } }
  class Params {
    constructor(value = {}) {
      this.values = new Map(typeof value === 'string' ? value.split('&').filter(Boolean).map(part => { const at = part.indexOf('='); return [part.slice(0, at), decodeURIComponent(part.slice(at + 1).replace(/\+/g, ' '))]; }) : Object.entries(value));
    }
    get(key) { return this.values.get(key); }
    set(key, value) { this.values.set(key, value); }
    toString() { return [...this.values].map(([key, value]) => `${encodeURIComponent(key)}=${encodeURIComponent(value)}`).join('&'); }
  }
  async function boot(options = {}) {
    const elements = new Map(), timers = new Map();
    let serial = 0, socket;
    const document = new Target();
    document.getElementById = id => {
      if (!elements.has(id)) elements.set(id, new Element());
      return elements.get(id);
    };
    document.documentElement = { dataset: {} };
    document.createElement = () => new Element();
    document.createTextNode = text => text;
    const get = document.getElementById;
    get('theme').options = [{ value: 'default' }]; get('font').options = [{ value: 'sans' }];
    for (const id of ['completions', 'queue', 'uploads', 'latest', 'toast']) get(id).hidden = true;
    class Socket extends Target {
      static OPEN = 1;
      constructor() { super(); this.readyState = 1; this.sent = []; socket = this; }
      send(frame) { this.sent.push(new Params(frame)); }
      close() { this.readyState = 0; }
    }
    const env = {
      document, WebSocket: Socket, TextEncoder: Encoder, TextDecoder: Decoder, URLSearchParams: Params,
      URL: class { constructor() { this.searchParams = new Params(); } static createObjectURL() { return 'blob:test-image'; } static revokeObjectURL() {} }, location: { href: 'http://localhost/', protocol: 'http:' },
      crypto: { getRandomValues(array) { return array.fill(++serial); } }, localStorage: { getItem() {}, setItem() {} },
      ResizeObserver: class { observe() {} }, performance: { now: () => 0 },
      MarkdownStream: class { write() {} end() {} },
      fetch: async () => ({ ok: true, json: async () => ({ images: options.images ? '1' : '0' }) }),
      setTimeout: (fn, delay) => { const id = ++serial; timers.set(id, { fn, delay }); return id; },
      clearTimeout: id => timers.delete(id)
    };
    const AsyncFunction = Object.getPrototypeOf(async function() {}).constructor;
    assert(source.startsWith("import { MarkdownStream } from './markdown.js';"), 'unexpected client module dependency');
    await new AsyncFunction(...Object.keys(env), source.replace(/^import[^\n]+\n/, ''))(...Object.values(env));
    socket.fire('open');
    const prompt = get('prompt');
    function input(text, inputType = 'insertText') {
      prompt.fire('beforeinput', { inputType });
      prompt.setRangeText(text, prompt.selectionStart, prompt.selectionEnd, 'end');
      prompt.fire('input', { inputType, data: text });
    }
    function key(key, fields = {}) {
      const event = prompt.fire('keydown', { key, ...fields });
      if (!event.defaultPrevented && (key === ' ' || (key === 'Enter' && fields.shiftKey))) input(key === ' ' ? ' ' : '\n');
    }
    function flush() {
      for (const [id, timer] of [...timers]) if (timer.delay <= 60) { timers.delete(id); timer.fn(); }
    }
    function reply(value, extra = {}) {
      const query = socket.sent.filter(frame => frame.get('action') === 'complete').at(-1);
      assert(query, 'completion query missing');
      socket.fire('message', { data: JSON.stringify({ type: 'complete', request: query.get('request'), label: value, value, start: '0', end: query.get('cursor'), continue: value.endsWith('/') ? '1' : '0', ...extra }) });
    }
    function sent(action = 'send') { return socket.sent.filter(frame => frame.get('action') === action).at(-1)?.get('text'); }
    return { get, prompt, input, key, flush, reply, sent, socket, form: () => get('composer').fire('submit') };
  }
  async function check(name, fn, options) {
    const ui = await boot(options);
    await fn(ui); checks.push(name);
  }
  await check('typed filename marker is removed before send-button submission', ui => {
    ui.input('@README.md'); ui.form(); assert(ui.sent() === 'README.md', 'file marker leaked to send payload');
  });
  await check('ordinary character-by-character typing retains the correct marker range', ui => {
    for (const character of '@README.md') ui.input(character);
    ui.form(); assert(ui.sent() === 'README.md', 'typing shifted the marker anchor');
  });
  await check('steer commits a directory marker without waiting for completion replies', ui => {
    ui.input('@examples/'); ui.key('Enter', { altKey: true }); assert(ui.sent('steer') === 'examples/', 'file marker leaked to steer payload');
  });
  await check('Enter commits a typed file before an asynchronous reply arrives', ui => {
    ui.input('@README.md'); ui.key('Enter'); assert(ui.sent() === 'README.md', 'Enter retained the file marker');
  });
  await check('typing invalidates old menu ranges and late completion replies', ui => {
    ui.input('@'); ui.flush(); ui.reply('README.md', { label: '@README.md', end: '1', continue: '0' });
    ui.input('REA'); ui.reply('README.md', { label: '@README.md', end: '1', continue: '0' }); ui.key('Enter');
    assert(ui.sent() === 'REA', 'stale candidate intercepted submission or used an old range');
  });
  await check('a bare completion marker does not submit an empty prompt', ui => {
    ui.input('@'); ui.form(); assert(ui.sent() === undefined && ui.prompt.value === '', 'bare marker sent an empty request');
  });
  await check('space commits only the active marker and preserves literal at-signs', ui => {
    ui.input('mail user@example.com '); ui.input('@icon@2x.png'); ui.key(' '); ui.input('please'); ui.form();
    assert(ui.sent() === 'mail user@example.com icon@2x.png please', 'literal at-sign changed or active marker retained');
  });
  await check('multiple file references are committed independently', ui => {
    ui.input('@README.md'); ui.key(' '); ui.input('@examples/chat.c'); ui.form();
    assert(ui.sent() === 'README.md examples/chat.c', 'a previous file marker remained');
  });
  await check('new-line completion exit strips the active marker', ui => {
    ui.input('@README.md'); ui.key('Enter', { shiftKey: true }); ui.input('next'); ui.form(); assert(ui.sent() === 'README.md\nnext', 'newline retained the marker');
  });
  await check('input-method whitespace commits a marker without a keydown event', ui => {
    ui.input('@README.md'); ui.input(' '); ui.input('next'); ui.form(); assert(ui.sent() === 'README.md next', 'input-method separator retained the marker');
  });
  await check('selected directory remains expandable but is committed on submit', ui => {
    ui.input('@exam'); ui.flush(); ui.reply('@examples/'); ui.get('completions').children[0].click();
    assert(ui.prompt.value === '@examples/', 'directory trigger removed before continuation'); ui.form(); assert(ui.sent() === 'examples/', 'selected directory marker leaked');
  });
  await check('selected file uses canonical path and preserves surrounding Unicode text', ui => {
    ui.input('€  tail', 'insertFromPaste'); ui.prompt.setSelectionRange(2, 2); ui.input('@REA'); ui.flush();
    ui.reply('README.md', { start: '4', end: '8', continue: '0' }); ui.get('completions').children[0].click(); ui.form();
    assert(ui.sent() === '€ README.md tail', 'byte offsets changed surrounding text');
  });
  await check('directory names containing spaces retain their completion range', ui => {
    ui.input('@spa'); ui.flush(); ui.reply('@space dir/'); ui.get('completions').children[0].click(); ui.input('fi'); ui.flush();
    const query = ui.socket.sent.filter(frame => frame.get('action') === 'complete').at(-1);
    assert(query.get('text') === '@space dir/fi' && query.get('start') === '0', 'directory range lost at internal space');
    ui.key('Enter', { altKey: true }); assert(ui.sent('steer') === 'space dir/fi', 'spaced directory marker leaked');
  });
  await check('a matching filename space continues completion instead of committing it', ui => {
    ui.input('@space'); ui.flush(); ui.reply('@space dir/'); ui.key(' '); ui.input('dir/'); ui.flush();
    const query = ui.socket.sent.filter(frame => frame.get('action') === 'complete').at(-1);
    assert(query.get('text') === '@space dir/', 'matching filename space ended completion');
    ui.form(); assert(ui.sent() === 'space dir/', 'directory marker leaked after a matching space');
  });
  await check('literal pasted text is not rewritten as file completion', ui => {
    ui.input('literal @mention user@example.com @README.md', 'insertFromPaste'); ui.form(); assert(ui.sent() === 'literal @mention user@example.com @README.md', 'pasted literal text changed');
  });
  await check('selected filenames retain their own leading at-sign', ui => {
    ui.input('@@li'); ui.flush(); ui.reply('@literal.txt', { label: '@@literal.txt', continue: '0' }); ui.get('completions').children[0].click();
    ui.input('x'); ui.form(); assert(ui.sent() === '@literal.txtx', 'selected literal at-sign became a trigger');
  });
  await check('opening the plus menu commits an existing directory marker', ui => {
    ui.input('@examples/'); ui.get('plus').click(); ui.reply('@other/', { start: '0', end: '9', continue: '1' }); ui.get('completions').children[0].click(); ui.form();
    assert(ui.sent() === 'other/', 'previous completion marker leaked through the plus menu');
  });
  await check('directory continuation leaves the all-completions menu mode', ui => {
    ui.get('plus').click(); ui.reply('@examples/', { start: '0', end: '0', continue: '1' });
    ui.get('completions').children[1].click(); ui.reply('examples/chat.c', { label: '@examples/chat.c', start: '0', end: '10', continue: '0' });
    assert(ui.get('completions').children.length === 1, 'directory continuation retained unrelated plus-menu items');
    ui.get('completions').children[0].click(); ui.form(); assert(ui.sent() === 'examples/chat.c', 'continued file path retained its trigger');
  }, { images: true });
  await check('nonbreaking spaces do not create a TUI-incompatible word boundary', ui => {
    ui.input('literal\u00a0@README.md'); ui.form(); assert(ui.sent() === 'literal\u00a0@README.md', 'nonbreaking-space text was rewritten');
  });
  await check('replaced draft cannot retain an old completion marker', ui => {
    ui.input('@README.md'); ui.prompt.setSelectionRange(0, ui.prompt.value.length); ui.input('plain user@example.com'); ui.form(); assert(ui.sent() === 'plain user@example.com', 'old marker affected replacement');
  });
  await check('leaving completion before editing earlier text preserves the cursor and path', ui => {
    ui.input('prefix @README.md'); ui.prompt.setSelectionRange(0, 0); ui.prompt.fire('click'); ui.input('X '); ui.form();
    assert(ui.sent() === 'X prefix README.md', 'leaving completion changed the insertion point');
  });
  await check('committing a file marker cannot cancel an image at the same insertion point', async ui => {
    ui.get('image-input').files = [{ name: 'test.jpg', size: 3, slice: () => ({ arrayBuffer: async () => new Uint8Array([255,216,255]).buffer }) }];
    ui.get('image-input').fire('change');
    for (let i = 0; i < 100 && !ui.socket.sent.some(frame => frame.get('action') === 'image_begin'); ++i) await Promise.resolve();
    assert(ui.socket.sent.some(frame => frame.get('action') === 'image_begin'), 'image upload did not begin');
    ui.input('@README.md'); ui.key(' ');
    assert(!ui.socket.sent.some(frame => frame.get('action') === 'image_cancel'), 'marker normalization cancelled an image');
    ui.key('c', { ctrlKey: true }); await Promise.resolve();
  }, { images: true });
  return checks;
}
