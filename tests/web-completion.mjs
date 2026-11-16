// Logical client tests: execute the shipped module with DOM/transport doubles.
// Actual browser layout and selection integration remain in web-browser.html.
export async function runWebCompletionTests(source, completionSource, promptSource, fixture) {
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
  const rows = fixture.split('\n').filter(line => line && !line.startsWith('#')).map(line => line.split('\t'));
  const policies = rows.filter(row => row[0] === 'provider');
  const catalog = new Map();
  for (const [, prefix, value] of rows.filter(row => row[0] === 'candidate')) {
    if (!catalog.has(prefix)) catalog.set(prefix, []);
    catalog.get(prefix).push(value);
  }
  const stripImports = source => source.replace(/^import[^\n]*\n/gm, '').replace(/^export /gm, '');
  const Completion = new Function(stripImports(completionSource) + '\nreturn Completion;')();
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
    for (const id of ['completions', 'prompt-projection', 'queue', 'uploads', 'latest', 'toast']) get(id).hidden = true;
    function event(value) { socket.fire('message', { data: JSON.stringify(value) }); }
    function reply(query, values, status = 'ok') {
      const prefix = query.get('resolve'), request = query.get('request');
      const items = values || (query.get('all') === '1' ? ['/approve', '/about', '@'] : catalog.get(prefix) || []);
      for (const candidate of items) event({
        type: 'complete', request, candidate, label: candidate,
        value: candidate.startsWith('@') && !candidate.endsWith('/') && candidate !== '@' ? candidate.slice(1) : candidate,
        start: query.get('all') === '1' ? '0' : query.get('start'),
        end: query.get('cursor'), continue: candidate.endsWith('/') || candidate === '@' ? '1' : '0'
      });
      event({ type: 'complete_end', request, status });
    }
    class Socket extends Target {
      static OPEN = 1;
      constructor() { super(); this.readyState = 1; this.sent = []; socket = this; }
      send(frame) {
        const query = new Params(frame); this.sent.push(query);
        if (query.get('action') === 'complete' && !options.manual) Promise.resolve().then(() => reply(query, undefined, options.failure ? 'error' : 'ok'));
      }
      close() { this.readyState = 0; this.fire('close'); }
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
    const names = Object.keys(env), values = Object.values(env);
    env.Prompt = new Function('Completion', ...names, stripImports(promptSource) + '\nreturn Prompt;')(Completion, ...values);
    const AsyncFunction = Object.getPrototypeOf(async function() {}).constructor;
    await new AsyncFunction(...Object.keys(env), stripImports(source))(...Object.values(env));
    socket.fire('open');
    for (const [, trigger, scope, strip, expand] of policies) event({ type: 'completion_provider', trigger, scope, strip, expand });
    const prompt = get('prompt');
    function input(text, inputType = 'insertText') {
      const e = prompt.fire('beforeinput', { inputType, data: text, cancelable: true });
      if (!e.defaultPrevented) {
        prompt.setRangeText(text, prompt.selectionStart, prompt.selectionEnd, 'end');
        prompt.fire('input', { inputType, data: text });
      }
    }
    function key(key, fields = {}) {
      const e = prompt.fire('keydown', { key, ...fields });
      if (!e.defaultPrevented) {
        if (key === ' ' || (key === 'Enter' && fields.shiftKey)) input(key === ' ' ? ' ' : '\n');
        if (key === 'Backspace' || key === 'Delete') {
          const start = prompt.selectionStart, end = prompt.selectionEnd, backward = key === 'Backspace';
          prompt.fire('beforeinput', { inputType: backward ? 'deleteContentBackward' : 'deleteContentForward' });
          prompt.setRangeText('', start === end && backward ? Math.max(0, start - 1) : start, start === end && !backward ? Math.min(prompt.value.length, end + 1) : end, 'end');
          prompt.fire('input', { inputType: backward ? 'deleteContentBackward' : 'deleteContentForward' });
        }
      }
      prompt.fire('keyup', { key, ...fields });
    }
    async function idle() { for (let i = 0; i < 150; ++i) await Promise.resolve(); }
    async function type(text) { for (const c of text) { input(c); await idle(); } }
    function queries() { return socket.sent.filter(frame => frame.get('action') === 'complete'); }
    function sent(action = 'send') { return socket.sent.filter(frame => frame.get('action') === action).at(-1)?.get('text'); }
    const ui = { get, prompt, input, type, key, idle, queries, reply, event, sent, socket,
      form: () => get('composer').fire('submit'), ghost: () => get('completion-ghost').textContent || '',
      timeout: delay => { for (const [id, timer] of [...timers]) if (timer.delay === delay) { timers.delete(id); timer.fn(); } } };
    return ui;
  }
  async function check(name, fn, options) {
    const ui = await boot(options);
    try { await fn(ui); }
    catch (error) { throw new Error(name + ': ' + error.message); }
    checks.push(name);
  }
  for (const [, name, script, expected] of rows.filter(row => row[0] === 'case')) {
    await check('native parity: ' + name, async ui => {
      for (const token of script.match(/<[^>]+>|[^<]/gu)) {
        const keys = { '<Enter>': 'Enter', '<Tab>': 'Tab', '<Space>': ' ', '<Right>': 'ArrowRight', '<Left>': 'ArrowLeft', '<Home>': 'Home', '<End>': 'End', '<Backspace>': 'Backspace', '<Delete>': 'Delete' };
        if (token === '<CtrlF>') ui.key('f', { ctrlKey: true });
        else if (keys[token]) ui.key(keys[token]);
        else ui.input(token);
        await ui.idle();
        assert(ui.get('completions').hidden, 'typing or a completion key opened a popup');
      }
      assert(ui.sent() === expected, 'got [' + ui.sent() + '], expected [' + expected + ']');
    });
  }
  await check('ghost is inline, display-only and follows typing without repeated queries', async ui => {
    ui.input('@'); await ui.idle();
    assert(ui.prompt.value === '@' && ui.ghost() === 'lua/', 'ghost mutated draft or was missing');
    ui.input('l'); await ui.idle();
    assert(ui.ghost() === 'ua/' && ui.queries().length === 1, 'provider snapshot was not reused');
    ui.input('i'); await ui.idle();
    assert(ui.ghost() === 'b/', 'typed prefix did not rematch cached inventory');
    ui.key('Enter'); await ui.idle(); assert(ui.sent() === 'li', 'Enter accepted ghost');
  });
  await check('committing directory previews children without a popup or insertion', async ui => {
    await ui.type('@l'); ui.key('Tab'); await ui.idle(); ui.key('ArrowRight'); await ui.idle();
    assert(ui.prompt.value === '@lua/' && ui.ghost() === 'source.c', 'Right inserted child or lost preview');
    assert(ui.get('completions').hidden && ui.queries().length === 2, 'continuation opened popup or eagerly resolved descendants');
  });
  await check('Tab cycle stays in parent inventory and never resolves children', async ui => {
    await ui.type('@l'); ui.key('Tab'); await ui.idle(); ui.key('Tab'); await ui.idle();
    assert(ui.prompt.value === '@lib/' && ui.queries().length === 1, 'Tab descended or lost original prefix');
  });
  await check('native Escape clears cycling without reverting inserted text', async ui => {
    await ui.type('@l'); ui.key('Tab'); await ui.idle(); ui.key('Escape');
    assert(ui.prompt.value === '@lua/', 'Escape reverted accepted text');
    ui.key('Enter'); await ui.idle(); assert(ui.sent() === 'lua/', 'Escape retained trigger');
  });
  await check('Enter submits immediately before root reply arrives', async ui => {
    ui.input('@REA'); ui.key('Enter'); await ui.idle();
    assert(ui.sent() === 'REA', 'Enter waited for or accepted an unresolved ghost');
    ui.reply(ui.queries()[0]); await ui.idle(); assert(!ui.ghost(), 'stale result reactivated submitted draft');
  }, { manual: true });
  await check('pending Tab preserves fast subsequent typing and submission order', async ui => {
    ui.input('@l'); ui.key('Tab'); ui.input('X'); ui.key('Enter');
    assert(ui.sent() === undefined, 'submission raced completion acceptance');
    ui.reply(ui.queries()[0]); await ui.idle(); assert(ui.sent() === 'lua/X', 'queued editor input was lost or reordered');
  }, { manual: true });
  await check('clicking away cancels pending acceptance', async ui => {
    ui.input('@l'); ui.key('Tab'); ui.prompt.setSelectionRange(0, 0); ui.prompt.fire('click');
    ui.reply(ui.queries()[0]); await ui.idle();
    assert(ui.prompt.value === 'l', 'late acceptance overwrote moved draft');
  }, { manual: true });
  await check('provider timeout releases pending editor input without losing typed text', async ui => {
    ui.input('@l'); ui.key('Tab'); ui.input('X'); ui.key('Enter'); ui.timeout(3000); await ui.idle();
    assert(ui.sent() === '@lX', 'failed provider changed trigger or lost subsequent input');
  }, { manual: true });
  await check('failed root provider preserves literal trigger', async ui => {
    await ui.type('@REA'); ui.form(); await ui.idle(); assert(ui.sent() === '@REA', 'failed provider stripped trigger');
  }, { failure: true });
  await check('literal pasted text does not activate any provider', async ui => {
    ui.input('literal @mention user@example.com @README.md', 'insertFromPaste'); ui.form(); await ui.idle();
    assert(ui.sent() === 'literal @mention user@example.com @README.md' && !ui.queries().length, 'paste was treated as completion syntax');
  });
  await check('a bare trigger does not send an empty prompt', async ui => {
    ui.input('@'); ui.form(); await ui.idle(); assert(ui.sent() === undefined && !ui.prompt.value, 'empty prompt submitted');
  });
  await check('steering strips only active trigger', async ui => {
    await ui.type('email user@example.com @icon@2'); ui.key('Enter', { altKey: true }); await ui.idle();
    assert(ui.sent('steer') === 'email user@example.com icon@2', 'steer changed literal at-signs');
  });
  await check('input-method separator follows the phrase completion rule', async ui => {
    await ui.type('@README.md'); ui.input(' '); ui.input('next'); ui.form(); await ui.idle();
    assert(ui.sent() === 'README.md next', 'input-method separator did not commit active trigger');
  });
  await check('new line commits current completion and keeps ordinary text', async ui => {
    await ui.type('@README.md'); ui.key('Enter', { shiftKey: true }); ui.input('next'); ui.form(); await ui.idle();
    assert(ui.sent() === 'README.md\nnext', 'newline lost text or retained trigger');
  });
  await check('replacement draft cannot retain an old activation', async ui => {
    await ui.type('@REA'); ui.prompt.setSelectionRange(0, ui.prompt.value.length); ui.input('plain user@example.com');
    ui.form(); await ui.idle(); assert(ui.sent() === 'plain user@example.com', 'old activation changed replacement');
  });
  await check('Unicode text surrounding a completed word is preserved', async ui => {
    ui.input('€  tail', 'insertFromPaste'); ui.prompt.setSelectionRange(2, 2);
    await ui.type('@REA'); ui.key('Tab'); await ui.idle(); ui.form(); await ui.idle();
    assert(ui.sent() === '€ README.md tail', 'completion changed surrounding Unicode or suffix');
  });
  await check('plus is the only explicit menu and directory choice returns to inline completion', async ui => {
    assert(ui.get('completions').hidden, 'menu visible initially');
    ui.get('plus').click(); await ui.idle();
    assert(!ui.get('completions').hidden, '+ did not open menu');
    ui.get('completions').children.at(-1).click(); await ui.idle();
    assert(ui.prompt.value === '@' && ui.get('completions').hidden && ui.ghost() === 'lua/', 'directory choice retained popup');
    ui.key('Tab'); await ui.idle(); ui.form(); await ui.idle(); assert(ui.sent() === 'lua/', 'plus choice retained trigger');
  }, { images: true });
  await check('same-position marker removal does not cancel an image upload', async ui => {
    ui.get('image-input').files = [{ name: 'test.jpg', size: 3, slice: () => ({ arrayBuffer: async () => new Uint8Array([255,216,255]).buffer }) }];
    ui.get('image-input').fire('change'); await ui.idle();
    assert(ui.socket.sent.some(frame => frame.get('action') === 'image_begin'), 'image upload did not begin');
    await ui.type('@README.md'); ui.key(' '); await ui.idle();
    assert(!ui.socket.sent.some(frame => frame.get('action') === 'image_cancel'), 'syntax-only commit cancelled image');
    ui.key('c', { ctrlKey: true }); await ui.idle();
    assert(ui.socket.sent.some(frame => frame.get('action') === 'image_cancel'), 'Ctrl-C did not cancel image');
  }, { images: true });
  await check('a complete provider snapshot can exceed 256 candidates', async ui => {
    ui.input('@item299');
    ui.reply(ui.queries()[0], Array.from({ length: 300 }, (_, n) => '@item' + String(n).padStart(3, '0') + '.txt'));
    await ui.idle(); assert(ui.ghost() === '.txt', 'inventory was truncated');
    ui.key('Tab'); await ui.idle(); ui.form(); await ui.idle();
    assert(ui.sent() === 'item299.txt', 'candidate beyond old cutoff was lost');
  }, { manual: true });
  await check('invalid provider strings fail without accepting or stripping syntax', async ui => {
    ui.input('@l'); ui.reply(ui.queries()[0], ['@lua/\u0000bad']); await ui.idle();
    assert(!ui.ghost(), 'invalid candidate rendered');
    ui.form(); await ui.idle(); assert(ui.sent() === '@l', 'invalid provider stripped marker');
  }, { manual: true });
  await check('selecting text hides the ghost and collapsing selection restores it', async ui => {
    await ui.type('@l'); ui.prompt.setSelectionRange(1, 2); ui.prompt.fire('select');
    assert(!ui.ghost() && ui.prompt.value === '@l', 'selection retained a display-only suggestion');
    ui.prompt.setSelectionRange(2, 2); ui.prompt.fire('select');
    assert(ui.ghost() === 'ua/', 'selection collapse did not rematch cached inventory');
  });
  await check('host output remains live while Tab waits for its provider', async ui => {
    ui.input('@l'); ui.key('Tab');
    ui.event({ type: 'status', busy: '1', text: 'Still streaming' });
    assert(ui.get('activity').textContent === 'Still streaming', 'pending editor key blocked host output');
    ui.reply(ui.queries()[0]); await ui.idle(); ui.form(); await ui.idle();
    assert(ui.sent() === 'lua/', 'provider completion failed after live host event');
  }, { manual: true });
  await check('late Right completion cannot move the cursor after a click elsewhere', async ui => {
    ui.input('@l'); ui.key('ArrowRight'); ui.prompt.setSelectionRange(0, 0); ui.prompt.fire('click');
    ui.reply(ui.queries()[0]); await ui.idle();
    assert(ui.prompt.value === 'l' && ui.prompt.selectionStart === 0, 'pending Right moved the new insertion point');
  }, { manual: true });
  return checks;
}
