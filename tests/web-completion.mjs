// Logical client tests: execute the shipped module with DOM/transport doubles.
// Actual browser layout and selection integration remain in web-browser.html.
export async function runWebCompletionTests(source, completionSource, promptSource, fixture, imagesSource, viewerSource, panelSource, transcriptsSource, dividerSource, motionSource) {
  const checks = [];
  function assert(value, message) { if (!value) throw new Error(message); }
  class Target {
    constructor() { this.listeners = new Map(); }
    addEventListener(name, fn) { const list = this.listeners.get(name) || []; list.push(fn); this.listeners.set(name, list); }
    dispatchEvent(event) { return !this.fire(event.type, { detail: event.detail }).defaultPrevented; }
    fire(name, fields = {}) {
      const event = { target: this, key: '', altKey: false, shiftKey: false, ctrlKey: false, metaKey: false, isComposing: false, defaultPrevented: false, preventDefault() { this.defaultPrevented = true; }, stopImmediatePropagation() { this.stopped = true; }, ...fields };
      for (const fn of this.listeners.get(name) || []) { fn(event); if (event.stopped) break; }
      return event;
    }
  }
  class Element extends Target {
    constructor() {
      super(); this.value = ''; this.selectionStart = this.selectionEnd = 0;
      this.hidden = false; this.children = []; this.dataset = {}; this.options = [];
      this.style = {
        getPropertyValue(name) { return this[name] || ''; },
        setProperty(name, value) { this[name] = value; }
      };
      this.scrollHeight = this.clientHeight = 36; this.scrollTop = 0;
      this.classList = { add() {}, remove() {}, contains() { return false; } };
    }
    setAttribute(name, value) { this[name] = value; }
    removeAttribute(name) { delete this[name]; }
    append(...values) { for (const value of values) { if (value && typeof value === 'object') value.parent = this; this.children.push(value); } }
    replaceChildren(...values) { this.children = []; this.append(...values); }
    set textContent(text) { this.replaceChildren(String(text)); }
    get textContent() { return this.children.map(child => typeof child === 'string' ? child : child.textContent).join(''); }
    getBoundingClientRect() { return this.rect || { left: 0, top: 0, right: 400, bottom: 36, width: 400, height: 36 }; }
    getClientRects() { return this.rects || [this.getBoundingClientRect()]; }
    get childElementCount() { return this.children.length; }
    focus(options) { this.focusOptions = options; if (this.ownerDocument) this.ownerDocument.activeElement = this; this.fire('focus'); }
    showModal() { this.beforeModal = this.ownerDocument?.activeElement; this.open = true; }
    close() { this.open = false; this.fire('close'); this.beforeModal?.focus(); }
    contains(other) { return this === other || this.children.some(child => typeof child === 'object' && (child === other || child.contains?.(other))); }
    closest(selector) {
      if (selector.split(',').some(part => part.startsWith('#') ? part.slice(1) === this.id : part.startsWith('.') ? !part.includes(' ') && (this.className || '').split(' ').includes(part.slice(1)) : part.startsWith('[contenteditable]') ? this.contenteditable !== undefined && this.contenteditable !== 'false' : part.toUpperCase() === this.tagName)) return this;
      return this.parent?.closest(selector) || null;
    }
    scrollIntoView() {}
    setPointerCapture(id) { this.capturedPointer = id; }
    hasPointerCapture(id) { return this.capturedPointer === id; }
    releasePointerCapture(id) { if (this.hasPointerCapture(id)) this.capturedPointer = null; }
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
  const ImageViewer = new Function(stripImports(viewerSource) + '\nreturn ImageViewer;')();
  const PaneDivider = new Function(stripImports(dividerSource) + '\nreturn PaneDivider;')();
  const TerminalPanel = new Function('PaneDivider', stripImports(panelSource) + '\nreturn TerminalPanel;')(PaneDivider);
  const { ImagePreviews, imageReferences } = new Function('ImageViewer', stripImports(imagesSource) + '\nreturn { ImagePreviews, imageReferences };')(ImageViewer);
  async function boot(options = {}) {
    const elements = new Map(), timers = new Map(), observers = [];
    let serial = 0, now = 0, socket, previews;
    const document = new Target(), window = new Target(), revoked = [], blobs = [], copiedImages = [], clipboardCalls = [], canvases = [], decoded = [], pendingDecodes = [], opened = [];
    const terminals = [], animations = [], splits = [];
    const Terminal = class {
      constructor(options) {
        this.options = options; this.chunks = []; this.osc = new Set();
        this.parser = { registerOscHandler: (code, fn) => { if (fn()) this.osc.add(code); } };
        terminals.push(this);
      }
      open(stage) { this.element = new Element(); stage.append(this.element); }
      get rows() { return this.options.rows; }
      write(bytes, done) { this.chunks.push(bytes); if (!options.stallTranscript) done(); }
      resize(cols, rows) { this.options.cols = cols; this.options.rows = rows; }
      refresh() {}
      dispose() { this.disposed = true; this.element?.remove(); }
    };
    const Transcripts = new Function('Terminal', 'PaneDivider', stripImports(transcriptsSource) + '\nreturn Transcripts;')(Terminal, PaneDivider);
    window.getComputedStyle = () => ({ getPropertyValue: () => '#141414' });
    window.open = (...args) => opened.push(args);
    window.innerWidth = 1024; window.innerHeight = 768;
    const narrow = new Target();
    narrow.matches = false;
    const reduced = new Target(); reduced.matches = !!options.reducedMotion;
    window.matchMedia = query => query === '(prefers-reduced-motion: reduce)' ? reduced : narrow;
    document.getElementById = id => {
      if (!elements.has(id)) {
        const element = new Element(); element.id = id; element.ownerDocument = document;
        element.tagName = { prompt: 'TEXTAREA', 'queue-edit-text': 'TEXTAREA', theme: 'SELECT', font: 'SELECT', margin: 'SELECT', width: 'SELECT', 'image-input': 'INPUT', 'queue-editor': 'DIALOG', 'image-viewer': 'DIALOG' }[id] || 'DIV';
        elements.set(id, element);
      }
      return elements.get(id);
    };
    document.documentElement = { dataset: {} };
    document.baseURI = options.baseURI;
    document.body = document.getElementById('body');
    document.activeElement = options.initialFocus ? document.getElementById(options.initialFocus) : document.body;
    document.hidden = !!options.hidden;
    document.querySelector = selector => selector === 'dialog[open]' ? [...elements.values()].find(element => element.tagName === 'DIALOG' && element.open) || null : null;
    document.createElement = tag => {
      const element = new Element(); element.ownerDocument = document; element.tagName = tag.toUpperCase();
      if (tag === 'iframe') element.contentWindow = { messages: [], postMessage(data, origin) { this.messages.push({ data, origin }); }, focus() { element.focus(); } };
      if (tag === 'canvas') {
        canvases.push(element);
        element.getContext = () => options.noCanvas ? null : { drawImage: image => { element.drawn = image; } };
        element.toBlob = callback => {
          if (options.canvasFailure) throw new Error('Canvas is not readable.');
          callback(options.canvasNull ? null : new window.Blob(['pixels'], { type: 'image/png' }));
        };
      }
      return element;
    };
    document.createTextNode = text => ({
      data: text, get textContent() { return this.data; },
      remove() { if (this.parent) this.parent.children = this.parent.children.filter(child => child !== this); }
    });
    const get = document.getElementById;
    get('transcripts').hidden = get('transcript-toggle').hidden = true;
    get('transcript-divider').hidden = get('terminal-divider').hidden = true;
    get('workspace').rect = { width: 1024, height: 704 };
    get('transcript-divider').rect = get('terminal-divider').rect = { width: 8, height: 8 };
    if (options.motion) for (const id of ['terminal-panel', 'terminal-divider', 'transcripts', 'transcript-divider']) {
      const element = get(id);
      element.getAnimations = () => splits.filter(animation => animation.element === element && !animation.cancelled);
      element.animate = (keyframes, timing) => {
        let resolve, reject;
        const animation = {
          element, keyframes, timing,
          finished: new Promise((done, cancelled) => { resolve = done; reject = cancelled; }),
          finish() { resolve(); },
          cancel() { this.cancelled = true; reject(new Error('animation cancelled')); }
        };
        splits.push(animation);
        return animation;
      };
    }
    get('copy-icon').content = { cloneNode: () => document.createElement('svg') };
    get('transcripts').append(get('transcript-tabs'), get('transcript-close'), get('transcript-screen'));
    get('conversation-pane').rect = { top: 64, height: 704 };
    get('composer-area').rect = { top: 600, height: 120 };
    if (options.motionLayout) {
      const chat = get('chat'), composer = get('composer-area');
      const classes = new Set(['welcome-state']);
      chat.classList = { add: name => classes.add(name), remove: name => classes.delete(name), contains: name => classes.has(name) };
      composer.getBoundingClientRect = () => ({ ...composer.rect, top: classes.has('welcome-state') ? 310 : composer.rect.top });
      composer.animate = (keyframes, timing) => animations.push({ keyframes, timing });
    }
    get('image-preview').append(get('image-preview-content'));
    get('theme').options = [{ value: 'default' }]; get('font').options = [{ value: 'sans' }];
    get('margin').options = ['none', 'small', 'medium', 'large', 'extra-large'].map(value => ({ value }));
    get('width').options = ['standard', 'medium', 'wide'].map(value => ({ value }));
    get('toolbar').append(get('brand'), get('theme'), get('font'), get('margin'), get('width'), get('transcript-toggle'), get('connection'));
    get('scroll-frame').append(get('scroll'));
    for (const id of ['completions', 'prompt-projection', 'queue', 'uploads', 'latest', 'toast', 'image-preview', 'terminal-panel']) get(id).hidden = true;
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
      constructor(url) { super(); this.url = url; this.readyState = 1; this.bufferedAmount = 0; this.sent = []; this.binary = []; socket = this; }
      send(frame) {
        if (typeof frame !== 'string') { this.binary.push(frame); return; }
        const query = new Params(frame);
        if (options.failSimulationReports && query.get('action') === 'simulation_status') throw new Error('status write failed');
        this.sent.push(query);
        if (query.get('action') === 'complete' && !options.manual) Promise.resolve().then(() => reply(query, undefined, options.failure ? 'error' : 'ok'));
      }
      close() { this.readyState = 0; this.fire('close'); }
    }
    const preferences = new Map(Object.entries(options.preferences || {}));
    const env = {
      document, window, TerminalPanel, Transcripts, ImagePreviews: class extends ImagePreviews { constructor(...args) { super(...args); previews = this; } },
      WebSocket: Socket, TextEncoder: Encoder, TextDecoder: Decoder, URLSearchParams: Params,
      URL: class extends globalThis.URL { static createObjectURL() { const url = 'blob:test-image-' + (++serial); blobs.push(url); return url; } static revokeObjectURL(url) { revoked.push(url); } }, location: { href: 'http://localhost/', protocol: 'http:', origin: 'http://localhost' },
      crypto: { getRandomValues(array) { return array.fill(++serial); } },
      localStorage: { getItem: name => preferences.get(name), setItem: (name, value) => preferences.set(name, value) },
      ResizeObserver: class { constructor(fn) { this.fn = fn; this.targets = []; observers.push(this); } observe(target) { this.targets.push(target); } }, performance: { now: () => now },
      MarkdownStream: class { write() {} end() {} },
      fetch: async () => { options.beforeConfig?.(document); return { ok: true, json: async () => ({ images: options.images ? '1' : '0', ...options.config }) }; },
      setTimeout: (fn, delay) => { const id = ++serial; timers.set(id, { fn, delay, due: now + delay }); return id; },
      clearTimeout: id => timers.delete(id)
    };
    window.URL = env.URL;
    window.location = env.location; window.ResizeObserver = env.ResizeObserver;
    window.setTimeout = env.setTimeout; window.clearTimeout = env.clearTimeout;
    window.Blob = class { constructor(parts, settings) { this.parts = parts; this.type = settings.type; } };
    window.Image = class {
      constructor() { this.naturalWidth = 640; this.naturalHeight = 480; }
      decode() {
        decoded.push(this);
        if (options.decodeFailure) return Promise.reject(new Error('Image cannot be decoded.'));
        return options.holdDecode ? new Promise(resolve => pendingDecodes.push(resolve)) : Promise.resolve();
      }
    };
    window.ClipboardItem = class {
      constructor(data) { this.data = data; this.types = Object.keys(data); }
      getType(type) { return Promise.resolve(this.data[type]); }
    };
    const copiedText = [];
    window.navigator = env.navigator = { clipboard: { writeText: async text => copiedText.push(text), write: async items => {
      clipboardCalls.push(items);
      if (options.clipboardFailure) throw new Error('Clipboard access denied.');
      for (const item of items) copiedImages.push(await item.getType('image/png'));
    } } };
    if (options.noImageClipboard) delete window.navigator.clipboard.write;
    const names = Object.keys(env), values = Object.values(env);
    env.Prompt = new Function('Completion', ...names, stripImports(promptSource) + '\nreturn Prompt;')(Completion, ...values);
    if (options.motion) new Function('document', 'window', motionSource)(document, window);
    const AsyncFunction = Object.getPrototypeOf(async function() {}).constructor;
    await new AsyncFunction(...Object.keys(env), stripImports(source))(...Object.values(env));
    socket.fire('open');
    event({ type: 'session', id: 'initial' });
    if (!options.noProviders) {
      for (const [, trigger, scope, strip, expand] of policies) event({ type: 'completion_provider', trigger, scope, strip, expand });
    }
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
        if (['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(key) && !fields.shiftKey) {
          const start = prompt.selectionStart, end = prompt.selectionEnd, text = prompt.value;
          let at = start;
          if (key === 'Home' || (fields.metaKey && key === 'ArrowLeft')) at = text.lastIndexOf('\n', start - 1) + 1;
          else if (key === 'End' || (fields.metaKey && key === 'ArrowRight')) { const next = text.indexOf('\n', end); at = next < 0 ? text.length : next; }
          else if (!fields.ctrlKey && !fields.altKey) at = key === 'ArrowLeft' ? start !== end ? start : Math.max(0, start - 1) : start !== end ? end : Math.min(text.length, end + 1);
          prompt.setSelectionRange(at, at);
        }
        if (key === ' ' || (key === 'Enter' && fields.shiftKey)) input(key === ' ' ? ' ' : '\n');
        if (key === 'Backspace' || key === 'Delete') {
          const start = prompt.selectionStart, end = prompt.selectionEnd, backward = key === 'Backspace';
          prompt.fire('beforeinput', { inputType: backward ? 'deleteContentBackward' : 'deleteContentForward' });
          prompt.setRangeText('', start === end && backward ? Math.max(0, start - 1) : start, start === end && !backward ? Math.min(prompt.value.length, end + 1) : end, 'end');
          prompt.fire('input', { inputType: backward ? 'deleteContentBackward' : 'deleteContentForward' });
        }
      }
      prompt.fire('keyup', { key, ...fields });
      return e;
    }
    async function idle() { for (let i = 0; i < 150; ++i) await Promise.resolve(); }
    async function type(text) { for (const c of text) { input(c); await idle(); } }
    function queries() { return socket.sent.filter(frame => frame.get('action') === 'complete'); }
    function sent(action = 'send') { return socket.sent.filter(frame => frame.get('action') === action).at(-1)?.get('text'); }
    async function upload(path, name = 'test.jpg', type = 'image/jpeg') {
      let read = false;
      const bytes = new Uint8Array(type === 'image/png' ? [137,80,78,71,13,10,26,10] : [255,216,255]);
      get('image-input').files = [{
        name, type, size: bytes.length, slice: () => ({ arrayBuffer: async () => bytes.buffer }),
        stream: () => ({ getReader: () => ({ read: async () => read ? { done: true } : (read = true, { done: false, value: bytes }), cancel: async () => {} }) })
      }];
      get('image-input').fire('change'); await idle();
      const begin = socket.sent.filter(frame => frame.get('action') === 'image_begin').at(-1);
      assert(begin, 'image upload did not start');
      const id = begin.get('id'), url = blobs.at(-1);
      event({ type: 'image_credit', id }); await idle();
      assert(socket.binary.length, 'image bytes did not use the upload stream');
      event({ type: 'image_credit', id }); await idle();
      assert(socket.sent.some(frame => frame.get('action') === 'image_end' && frame.get('id') === id), 'image stream did not finish');
      event({ type: 'image_done', id, path }); await idle();
      return { id, url, file: get('image-input').files[0] };
    }
    const ui = { get, prompt, input, type, key, idle, queries, reply, event, sent, get socket() { return socket; }, document, window, revoked, blobs, upload, previews, opened, terminals, animations, splits, reduced, preferences,
      socketURL: () => socket.url,
      copiedImages, copiedText, clipboardCalls, canvases, decoded, narrow, finishDecode: () => pendingDecodes.shift()?.(),
      resize: target => { for (const observer of observers) if (observer.targets.includes(target)) observer.fn(); },
      form: () => get('composer').fire('submit'), ghost: () => get('completion-ghost').textContent || '',
      timeout: delay => { for (const [id, timer] of [...timers]) if (timer.delay === delay) { timers.delete(id); timer.fn(); } },
      advance: delay => {
        const end = now + delay;
        for (;;) {
          const next = [...timers].filter(([, timer]) => timer.due <= end).sort((a, b) => a[1].due - b[1].due)[0];
          if (!next) break;
          const [id, timer] = next; now = timer.due; timers.delete(id); timer.fn();
        }
        now = end;
      } };
    return ui;
  }
  async function check(name, fn, options) {
    const ui = await boot(options);
    try { await fn(ui); }
    catch (error) { throw new Error(name + ': ' + error.message); }
    checks.push(name);
  }
  function queueState(ui, entries) {
    ui.event({ type: 'queue_begin' });
    for (const item of entries) ui.event({ type: 'queue_item', mode: 'queued', held: '1', text: 'Approval text', ...item });
    ui.event({ type: 'queue_end' });
  }
  await check('F9 and the actions menu hide and restore the toolbar without losing the draft', async ui => {
    const toolbar = ui.get('toolbar');
    await ui.type('preserve the draft'); ui.prompt.setSelectionRange(2, 6);
    ui.get('margin').focus();
    assert(!toolbar.hidden && !ui.window.fire('keydown', { key: 'F9', altKey: true }).defaultPrevented, 'modified F9 changed the toolbar');
    assert(ui.window.fire('keydown', { key: 'F9' }).defaultPrevented && toolbar.hidden &&
      ui.document.activeElement === ui.prompt && ui.preferences.get('softline.toolbarHidden') === '1', 'F9 did not hide the toolbar, restore focus or save the preference');
    ui.window.fire('keydown', { key: 'F9', repeat: true });
    assert(toolbar.hidden && ui.prompt.value === 'preserve the draft' && ui.prompt.selectionStart === 2 && ui.prompt.selectionEnd === 6, 'key repeat or focus transfer changed the draft');
    ui.event({ type: 'prompt_state', disabled: '1' });
    ui.get('plus').click();
    const restore = ui.get('completions').children.find(item => item.textContent.includes('Show toolbar'));
    assert(restore && !ui.get('plus').disabled, 'hidden toolbar could not be restored during a host lock');
    restore.click();
    assert(!toolbar.hidden && ui.get('plus').disabled && ui.prompt.readOnly &&
      ui.preferences.get('softline.toolbarHidden') === '0', 'menu restore changed the prompt lock or missed the preference');
  });
  await check('appearance controls load and save browser preferences', async ui => {
    const margin = ui.get('margin'), width = ui.get('width');
    assert(ui.get('toolbar').hidden && margin.options.length === 5 && margin.value === 'large' &&
      ui.document.documentElement.dataset.margin === 'large', 'saved appearance was not applied at startup');
    assert(width.value === 'wide' && ui.document.documentElement.dataset.width === 'wide', 'saved content width was not applied');
    ui.window.fire('keydown', { key: 'F9' });
    margin.value = 'small'; margin.fire('change');
    assert(ui.document.documentElement.dataset.margin === 'small' && ui.preferences.get('softline.margin') === 'small', 'margin change did not update or persist');
    width.value = 'medium'; width.fire('change');
    assert(ui.document.documentElement.dataset.width === 'medium' && ui.preferences.get('softline.width') === 'medium', 'width change did not update or persist');
  }, { preferences: { 'softline.toolbarHidden': '1', 'softline.margin': 'large', 'softline.width': 'wide' } });
  await check('an invalid saved width falls back to Standard', async ui => {
    assert(ui.get('width').value === 'standard' && ui.document.documentElement.dataset.width === 'standard', 'invalid width escaped the preset list');
  }, { preferences: { 'softline.width': 'other' } });
  for (const motion of [true, false]) await check(`top scroll edge follows overflow in the ${motion ? 'animated' : 'plain'} page`, async ui => {
    const scroll = ui.get('scroll'), frame = ui.get('scroll-frame');
    scroll.scrollHeight = 1000;
    scroll.scrollTop = 140; scroll.fire('scroll');
    assert(frame.dataset.scrolled === (motion ? '1' : undefined) && !ui.get('latest').hidden, 'scrolling did not expose the expected edges or jump arrow');
    ui.get('latest').click(); scroll.fire('scroll');
    assert(frame.dataset.scrolled === (motion ? '1' : undefined) && ui.get('latest').hidden, 'jump-to-latest retained the bottom edge trigger');
    scroll.scrollTop = 0; scroll.fire('scroll');
    assert(frame.dataset.scrolled === (motion ? '0' : undefined), 'top edge covered the start of the conversation');
  }, { motion });
  await check('the optional motion module docks the composer once after a successful first send', async ui => {
    await ui.type('initial draft');
    assert(!ui.animations.length && ui.get('chat').classList.contains('welcome-state'), 'typing moved the composer');
    ui.socket.close(); ui.key('Enter'); await ui.idle();
    assert(!ui.animations.length && ui.prompt.value === 'initial draft', 'failed send moved the composer or lost the draft');
    ui.timeout(1000); ui.event({ type: 'session', id: 'initial' });
    ui.key('Enter'); await ui.idle();
    assert(ui.sent() === 'initial draft' && ui.prompt.value === '' && ui.animations.length === 1, 'first send did not dock the composer');
    const { keyframes, timing } = ui.animations[0];
    assert(keyframes[0].translate === '0 -290px' && keyframes[1].translate === '0 0' && timing.duration === 540, 'dock did not animate from the measured welcome position');
    await ui.type('next prompt'); ui.key('Enter'); await ui.idle();
    assert(ui.sent() === 'next prompt' && ui.animations.length === 1, 'subsequent sends animated the already docked composer');
  }, { motion: true, motionLayout: true });
  for (const [name, motion, reducedMotion] of [['plain page', false, false], ['reduced motion', true, true]]) {
    await check(`${name} docks the composer immediately`, async ui => {
      await ui.type('initial draft'); ui.key('Enter'); await ui.idle();
      assert(ui.sent() === 'initial draft' && !ui.animations.length && !ui.get('chat').classList.contains('welcome-state'), 'submit did not dock without animation');
    }, { motion, reducedMotion, motionLayout: true });
  }
  await check('history replay docks the composer without flying from the welcome position', async ui => {
    ui.event({ type: 'delta', id: 'm1', role: 'user', text: 'Existing turn' });
    assert(!ui.get('chat').classList.contains('welcome-state') && !ui.animations.length, 'history triggered a local send animation');
    await ui.type('next prompt'); ui.key('Enter'); await ui.idle();
    assert(ui.sent() === 'next prompt' && !ui.animations.length, 'an already docked composer took the welcome flight');
  }, { motion: true, motionLayout: true });
  await check('a drained queue never appears and the next arrivals age independently', async ui => {
    const queue = ui.get('queue'), queued = { id: 'queued', held: '0' }, steer = { id: 'steer', held: '0', mode: 'steer' };
    await ui.type('sent immediately'); ui.key('Enter'); await ui.idle();
    assert(ui.sent() === 'sent immediately', 'display delay blocked prompt delivery');
    queueState(ui, [{ ...queued, id: 'quick' }]); ui.advance(30);
    assert(queue.hidden, 'short-lived entry appeared');
    queueState(ui, []); ui.advance(500);
    assert(queue.hidden, 'a consumed row appeared on its deadline');
    queueState(ui, [queued]); ui.advance(100); queueState(ui, [queued, steer]); ui.advance(399);
    assert(queue.hidden, 'queue appeared before 500 ms');
    ui.advance(1);
    assert(queue.children.length === 1 && queue.children[0].dataset.id === 'queued', 'first arrival did not age independently');
    const pending = { id: 'pending', held: '0', mode: 'pending' };
    queueState(ui, [steer, pending]);
    assert(queue.children.length === 1 && queue.children[0].dataset.id === 'pending', 'a visible queue delayed its newest arrival');
    ui.advance(100);
    assert(queue.children.length === 2, 'the older steering entry lost its original deadline');
  });
  await check('a custom delay starts when the complete queue snapshot arrives', async ui => {
    const queue = ui.get('queue');
    ui.event({ type: 'queue_begin' });
    ui.event({ type: 'queue_item', id: 'slow', held: '0', mode: 'pending' });
    ui.advance(125);
    ui.event({ type: 'queue_end' }); ui.advance(124);
    assert(queue.hidden, 'the timer elapsed while the snapshot was incomplete');
    ui.advance(1);
    assert(!queue.hidden && queue.children[0].dataset.id === 'slow', 'configured delay did not reveal a pending row');
  }, { config: { queue_delay: '125' } });
  await check('zero delay shows ordinary queue rows immediately', async ui => {
    queueState(ui, [{ id: 'queued', held: '0' }]);
    assert(!ui.get('queue').hidden, 'zero delay held a queue row');
  }, { config: { queue_delay: '0' } });
  await check('a waiting approval and connection errors bypass the queue delay', async ui => {
    queueState(ui, [{ id: 'approval', revision: '1' }, { id: 'queued', held: '0' }]);
    ui.event({ type: 'error', text: 'Connection failed' });
    assert(ui.get('queue').children.length === 1 && ui.get('queue').children[0].dataset.id === 'approval', 'approval was delayed');
    assert(!ui.get('toast').hidden && ui.get('toast').textContent === 'Connection failed', 'connection error was delayed');
  });
  const shortcut = (ui, action, extra = {}) => ui.key({ approvals: '!', approve: '@', reject: '#', edit: 'Enter', terminal: '`' }[action],
    { ctrlKey: true, shiftKey: action !== 'terminal', code: { approvals: 'Digit1', approve: 'Digit2', reject: 'Digit3', edit: 'Enter', terminal: 'Backquote' }[action], ...extra });
  await check('startup focuses the prompt before configuration or connection finishes', async ui => {
    assert(ui.document.activeElement === ui.prompt && ui.prompt.focusOptions.preventScroll, 'startup did not focus the prompt without scrolling');
  }, { beforeConfig(document) { assert(document.activeElement === document.getElementById('prompt'), 'startup focus waited for configuration'); } });
  function simulate(ui, text, pace, mode, id) {
    ui.event({ type: 'simulate_begin', start: '1000', pace, submit: '2000', mode, id });
    ui.event({ type: 'simulate_text', text, id });
    ui.event({ type: 'simulate_end', id });
  }
  function simulationReports(ui, id) {
    return ui.socket.sent.filter(frame => frame.get('action') === 'simulation_status' &&
      (id === undefined || frame.get('id') === id));
  }
  await check('simulated browser typing waits, inserts graphemes and steers once', async ui => {
    simulate(ui, 'e\u0301x', '30', 's');
    assert(!ui.prompt.value && !ui.sent('steer') && !ui.animations.length, 'initial grace was skipped');
    ui.timeout(1000);
    assert(ui.prompt.value === 'e\u0301', 'first visible character was split');
    ui.timeout(30);
    assert(ui.prompt.value === 'e\u0301x' && !ui.sent('steer') && !ui.animations.length, 'pre-submit grace was skipped or typing moved the composer');
    ui.timeout(2000); await ui.idle();
    assert(ui.sent('steer') === 'e\u0301x' && !ui.prompt.value &&
      ui.socket.sent.filter(frame => frame.get('action') === 'steer').length === 1,
      'simulation did not use the normal steer submission');
    assert(!simulationReports(ui).length, 'untracked simulation sent feedback');
    assert(ui.animations.length === 1, 'simulated submit did not use the normal composer motion');
  }, { motion: true, motionLayout: true });
  await check('tracked simulation reports scheduling and correlates the ordinary submission', async ui => {
    simulate(ui, 'tracked reply', '0', 's', 'run_17');
    await ui.idle();
    assert(simulationReports(ui, 'run_17').map(report => report.get('status')).join(',') === 'started' && !ui.prompt.value,
      'started was not reported during the grace period');
    ui.timeout(1000);
    assert(ui.prompt.value === 'tracked reply' && !ui.sent('steer'), 'feedback changed submission timing');
    ui.timeout(2000); await ui.idle();
    const reports = simulationReports(ui, 'run_17');
    const submitted = ui.socket.sent.find(frame => frame.get('action') === 'steer');
    assert(reports.length === 2 && reports[1].get('status') === 'submitted' &&
      submitted?.get('text') === 'tracked reply' && reports[1].get('request') === submitted.get('request') &&
      ui.socket.sent.indexOf(reports[1]) > ui.socket.sent.indexOf(submitted), 'feedback lost the normal submission identity or order');
    ui.event({ type: 'simulate_cancel', id: 'run_17' }); await ui.idle();
    assert(simulationReports(ui, 'run_17').length === 2 && ui.sent('steer') === 'tracked reply', 'late cancellation affected a submitted job');
  });
  await check('a tracked simulation reports a real draft without changing it', async ui => {
    await ui.type('keep this');
    simulate(ui, 'discard this', '0', 'q', 'occupied');
    await ui.idle();
    const reports = simulationReports(ui, 'occupied');
    assert(reports.length === 1 && reports[0].get('status') === 'declined' &&
      reports[0].get('reason') === 'draft_present' && ui.prompt.value === 'keep this' && !ui.sent(),
      'draft was changed or the decline was ambiguous');
  });
  await check('only matching frames and cancellation affect a tracked simulation', async ui => {
    ui.event({ type: 'simulate_begin', start: '1000', pace: '50', submit: '2000', mode: 'q', id: 'current' });
    ui.event({ type: 'simulate_text', text: 'wrong', id: 'stale' });
    ui.event({ type: 'simulate_end', id: 'stale' });
    ui.event({ type: 'simulate_text', text: 'abc', id: 'current' });
    ui.event({ type: 'simulate_end', id: 'current' });
    ui.timeout(1000);
    assert(ui.prompt.value === 'a', 'unrelated frames changed or started the draft');
    ui.event({ type: 'simulate_cancel', id: 'stale' }); ui.timeout(50);
    assert(ui.prompt.value === 'ab', 'stale cancellation stopped the active simulation');
    ui.event({ type: 'simulate_cancel', id: 'current' });
    ui.event({ type: 'simulate_cancel', id: 'current' });
    ui.timeout(50); ui.timeout(2000); await ui.idle();
    const reports = simulationReports(ui, 'current');
    assert(reports.map(report => report.get('status')).join(',') === 'started,cancelled' &&
      reports[1].get('reason') === 'host_cancelled' && !simulationReports(ui, 'stale').length &&
      ui.prompt.value === 'ab' && !ui.sent() && ui.get('toast').hidden,
      'cancellation discarded the text, sent it, emitted a toast or reported twice');
  });
  await check('host can cancel a tracked simulation before the last frame arrives', async ui => {
    ui.prompt.value = ' \t';
    ui.event({ type: 'simulate_begin', start: '1000', pace: '0', submit: '2000', mode: 'q', id: 'pending' });
    ui.event({ type: 'simulate_cancel', id: 'pending' });
    ui.event({ type: 'simulate_text', text: 'late text', id: 'pending' });
    ui.event({ type: 'simulate_end', id: 'pending' });
    ui.timeout(1000); ui.timeout(2000); await ui.idle();
    assert(ui.prompt.value === ' \t' && !ui.sent() &&
      simulationReports(ui, 'pending').map(report => report.get('status')).join(',') === 'cancelled',
      'incomplete cancelled job cleared the composer, started or submitted');
  });
  for (const congested of [false, true]) await check(`feedback ${congested ? 'backpressure' : 'write failure'} leaves submission alone`, async ui => {
    if (congested) ui.socket.bufferedAmount = 64 * 1024;
    simulate(ui, 'normal send', '0', 'q', 'best-effort');
    await ui.idle(); ui.timeout(1000); ui.timeout(2000); await ui.idle();
    assert(ui.sent() === 'normal send' && !simulationReports(ui).length && ui.get('toast').hidden,
      'failed telemetry interfered with normal submission or the UI');
  }, { failSimulationReports: !congested });
  await check('disconnect drops pending feedback and cancels automatic submission', async ui => {
    simulate(ui, 'kept draft', '0', 'q', 'interrupted'); ui.timeout(1000);
    ui.socket.close(); ui.event({ type: 'session', id: 'new-epoch' });
    ui.timeout(2000); await ui.idle();
    assert(!simulationReports(ui).length && !ui.sent() && ui.prompt.value === 'kept draft',
      'an obsolete report escaped into the new session or the draft was lost');
  });
  await check('tracked input takeover reports one cancellation and keeps the edited text', async ui => {
    simulate(ui, 'typed', '0', 'q', 'manual'); ui.timeout(1000); ui.input('!');
    ui.timeout(2000); await ui.idle();
    const reports = simulationReports(ui, 'manual');
    assert(reports.map(report => report.get('status')).join(',') === 'started,cancelled' &&
      reports[1].get('reason') === 'manual_input' && ui.prompt.value === 'typed!' && !ui.sent(),
      'manual takeover lost the draft or reported the wrong outcome');
  });
  await check('simulated characters resize the composer only when they need more room', async ui => {
    let height = ui.prompt.style.height;
    const changes = [];
    Object.defineProperty(ui.prompt.style, 'height', {
      get: () => height, set: value => { height = value; changes.push(value); }
    });
    simulate(ui, 'abc', '50', 'q');
    assert(!changes.length && !ui.prompt.value, 'arming the simulation resized the composer during grace');
    ui.timeout(1000);
    assert(!changes.length, 'a character on the same line reset the height');
    ui.prompt.scrollHeight = 60;
    ui.timeout(50); ui.timeout(50);
    assert(changes.join(',') === '60px', 'composer did not grow once at the wrap');
  });
  await check('the simulated caret follows the draft through both grace periods', async ui => {
    const composer = ui.get('composer'), before = ui.get('projection-before'), scroll = ui.get('scroll');
    scroll.focus();
    simulate(ui, 'a\nb', '50', 'q');
    assert(composer.dataset.simulating === '1' && !before.textContent, 'caret was absent at the start of grace');
    ui.timeout(1000); ui.timeout(50);
    assert(before.textContent === 'a\n', 'caret did not follow the newline');
    ui.timeout(50);
    assert(composer.dataset.simulating === '1' && before.textContent === 'a\nb' && !ui.sent(), 'caret was absent while waiting to submit');
    assert(ui.document.activeElement === scroll, 'simulated caret moved keyboard focus');
    ui.timeout(2000); await ui.idle();
    assert(composer.dataset.simulating !== '1' && ui.sent() === 'a\nb', 'caret stayed active after submission');
  });
  await check('a long simulated draft scrolls its caret into view', async ui => {
    ui.prompt.clientHeight = 200;
    simulate(ui, 'lines\n'.repeat(12), '0', 'q');
    ui.prompt.scrollHeight = 320;
    ui.timeout(1000);
    assert(ui.prompt.scrollTop > 0 && ui.get('prompt-projection').scrollTop === ui.prompt.scrollTop,
      'caret projection did not follow the scrolling textarea');
  });
  await check('a simulated send preserves the reading position and focus', async ui => {
    const scroll = ui.get('scroll');
    scroll.scrollHeight = 1000; scroll.clientHeight = 250; scroll.scrollTop = 123;
    scroll.fire('scroll'); scroll.focus();
    simulate(ui, 'read without jumping', '0', 'q');
    ui.timeout(1000); ui.timeout(2000); await ui.idle();
    ui.event({ type: 'begin', id: 'echo', role: 'user' }); ui.resize(scroll);
    assert(ui.sent() === 'read without jumping' && scroll.scrollTop === 123 &&
      ui.document.activeElement === scroll, 'automatic send moved the reader');
  });
  await check('unpaced browser simulation inserts all text and queues once', async ui => {
    simulate(ui, 'paste everything', '0', 'q');
    ui.timeout(1000);
    assert(ui.prompt.value === 'paste everything' && !ui.sent(), 'unpaced insertion or grace was wrong');
    ui.timeout(2000); await ui.idle();
    assert(ui.sent() === 'paste everything' && !ui.prompt.value, 'normal queue submission was skipped');
  });
  await check('simulation replaces a whitespace-only draft and sends its own text once', async ui => {
    ui.prompt.value = ' \t\n\u00a0 ';
    ui.prompt.setSelectionRange(ui.prompt.value.length, ui.prompt.value.length);
    simulate(ui, 'send this', '0', 'q');
    assert(!ui.prompt.value && ui.prompt.selectionStart === 0 && ui.get('toast').hidden,
      'whitespace was treated as a draft or the composer was not reset');
    assert(!ui.sent(), 'simulation skipped its initial grace');
    ui.timeout(1000);
    assert(ui.prompt.value === 'send this' && !ui.sent(), 'simulation kept old whitespace or skipped submit grace');
    ui.timeout(2000); await ui.idle();
    assert(ui.sent() === 'send this' && ui.socket.sent.filter(frame => frame.get('action') === 'send').length === 1,
      'simulation did not send exactly the supplied prompt once');
  });
  await check('an empty simulation leaves the composer alone', async ui => {
    ui.prompt.value = ' \t';
    simulate(ui, ' \n', '0', 'q');
    ui.timeout(1000); ui.timeout(2000); await ui.idle();
    assert(ui.prompt.value === ' \t' && !ui.sent(), 'an invalid simulation changed the composer');
  });
  await check('manual input and socket closure cancel simulated auto-submit', async ui => {
    simulate(ui, 'follows', '0', 'q');
    ui.timeout(1000); ui.input('!');
    ui.timeout(2000); await ui.idle();
    assert(!ui.sent() && ui.prompt.value === 'follows!', 'manual edit was submitted or lost');
    assert(ui.get('composer').dataset.simulating !== '1', 'manual input left the simulated caret visible');
    ui.prompt.value = '';
    simulate(ui, 'disconnecting', '0', 'q');
    ui.timeout(1000); ui.socket.close();
    assert(ui.get('composer').dataset.simulating !== '1', 'disconnect left the simulated caret visible');
    ui.event({ type: 'session', id: 'initial' });
    ui.timeout(2000); await ui.idle();
    assert(!ui.sent(), 'reconnection resumed unfinished automation');
  });
  await check('a prompt lock cancels simulation and existing drafts take priority', async ui => {
    await ui.type('keep this');
    simulate(ui, 'different', '0', 'q');
    ui.timeout(1000); ui.timeout(2000);
    assert(ui.prompt.value === 'keep this' && !ui.sent(), 'simulation replaced a draft');
    ui.prompt.value = '';
    simulate(ui, 'typing finished', '0', 'q');
    ui.timeout(1000);
    ui.event({ type: 'prompt_state', disabled: '1' });
    assert(ui.get('composer').dataset.simulating !== '1', 'prompt lock left the simulated caret visible');
    ui.event({ type: 'prompt_state', disabled: '0' });
    ui.timeout(2000); await ui.idle();
    assert(ui.prompt.value === 'typing finished' && !ui.sent(), 'unlock resumed cancelled submission');
  });
  for (const id of ['theme', 'font', 'queue-edit-text']) await check('startup and window return preserve focus on ' + id, async ui => {
    assert(ui.document.activeElement === ui.get(id), 'startup stole existing focus');
    ui.window.fire('focus');
    assert(ui.document.activeElement === ui.get(id), 'window return stole control focus');
    ui.event({ type: 'status', busy: '1', text: 'Thinking' });
    ui.event({ type: 'session', id: 'initial' });
    assert(ui.document.activeElement === ui.get(id), 'server updates stole focus');
  }, { initialFocus: id });
  await check('returning to an idle page restores prompt focus without altering its selection', async ui => {
    await ui.type('keep this draft'); ui.prompt.setSelectionRange(2, 7);
    ui.document.body.focus(); ui.window.fire('focus');
    assert(ui.document.activeElement === ui.prompt && ui.prompt.value === 'keep this draft' && ui.prompt.selectionStart === 2 && ui.prompt.selectionEnd === 7, 'window return altered the draft or selection');
  });
  await check('hidden pages wait until window return before focusing the prompt', async ui => {
    assert(ui.document.activeElement === ui.document.body, 'hidden startup focused the prompt');
    ui.document.hidden = false; ui.window.fire('focus');
    assert(ui.document.activeElement === ui.prompt, 'visible window did not regain prompt focus');
  }, { hidden: true });
  await check('Escape returns to the prompt without changing draft or cancelling generation', async ui => {
    await ui.type('keep this draft'); ui.prompt.setSelectionRange(2, 7);
    ui.event({ type: 'status', busy: '1', text: 'Thinking' });
    ui.get('scroll').scrollTop = 123; ui.get('scroll').focus();
    const event = ui.document.fire('keydown', { target: ui.get('scroll'), key: 'Escape' });
    assert(event.defaultPrevented && ui.document.activeElement === ui.prompt && ui.prompt.focusOptions.preventScroll, 'Escape did not focus without scrolling');
    assert(ui.prompt.value === 'keep this draft' && ui.prompt.selectionStart === 2 && ui.prompt.selectionEnd === 7 && ui.get('scroll').scrollTop === 123, 'Escape changed the draft, selection or conversation scroll');
    assert(!ui.socket.sent.some(frame => frame.get('action') === 'cancel'), 'returning focus cancelled generation');
    ui.get('scroll').focus();
    assert(ui.document.fire('keydown', { key: 'Escape' }).defaultPrevented && ui.document.activeElement === ui.prompt, 'document-level Escape failed to focus');
  });
  await check('Escape leaves other editors, selectors, modifiers and composition alone', async ui => {
    for (const id of ['theme', 'font', 'queue-edit-text', 'image-input', 'host-editable']) {
      const control = ui.get(id);
      if (id === 'host-editable') control.setAttribute('contenteditable', 'true');
      control.focus();
      const event = ui.document.fire('keydown', { target: control, key: 'Escape' });
      assert(!event.defaultPrevented && ui.document.activeElement === control, 'Escape stole focus from ' + id);
    }
    for (const fields of [{ ctrlKey: true }, { altKey: true }, { metaKey: true }, { shiftKey: true }, { isComposing: true }, { defaultPrevented: true }]) {
      ui.get('scroll').focus();
      ui.document.fire('keydown', { target: ui.get('scroll'), key: 'Escape', ...fields });
      assert(ui.document.activeElement === ui.get('scroll'), 'modified or handled Escape stole focus');
    }
  });
  for (const id of ['queue-editor', 'image-viewer']) await check('open ' + id + ' keeps focus during window return and Escape', async ui => {
    const dialog = ui.get(id); dialog.showModal(); dialog.focus();
    ui.window.fire('focus');
    const event = ui.document.fire('keydown', { target: dialog, key: 'Escape' });
    assert(!event.defaultPrevented && dialog.open && ui.document.activeElement === dialog, 'focus recovery interfered with the modal');
  });
  function openTerminal(ui) {
    ui.event({ type: 'capabilities', terminal: '/terminal', edit: '1' }); shortcut(ui, 'terminal');
    return ui.get('terminal-panel').children[0];
  }
  function terminalEvent(ui, data, source = ui.get('terminal-panel').children[0]?.contentWindow, origin = 'http://localhost') {
    ui.window.fire('message', { data, source, origin });
  }
  function transcript(ui, id, type = 'open', fields = {}) {
    ui.event({ type: 'transcript_' + type, id, title: id, rows: '24', columns: '80', ...fields });
  }
  await check('chat appearance changes preserve terminal and transcript visibility', async ui => {
    const frame = openTerminal(ui), terminal = ui.get('terminal-panel'), viewer = ui.get('transcripts');
    transcript(ui, 'log'); ui.get('transcript-toggle').click();
    const margin = ui.get('margin'), width = ui.get('width');
    margin.value = 'extra-large'; margin.fire('change');
    width.value = 'wide'; width.fire('change');
    assert(!terminal.hidden && terminal.children[0] === frame && !viewer.hidden, 'chat appearance changed an open pane');
    shortcut(ui, 'terminal');
    assert(terminal.hidden && !viewer.hidden, 'margin coupled the terminal and transcript');
    ui.get('transcript-close').click();
    assert(viewer.hidden && ui.document.documentElement.dataset.margin === 'extra-large' &&
      ui.document.documentElement.dataset.width === 'wide', 'closing a pane reset chat appearance');
  });
  await check('animated shell close stops the transport immediately and protects a reopened split', async ui => {
    const shell = openTerminal(ui), panel = ui.get('terminal-panel'), divider = ui.get('terminal-divider');
    terminalEvent(ui, { type: 'softline-terminal-size', rowHeight: 18, chromeHeight: 40 });
    panel.rect = { height: 170 };
    shortcut(ui, 'terminal');
    assert(shell.contentWindow.messages.some(message => message.data.type === 'softline-terminal-stop') &&
      !panel.hidden && panel.inert && divider.inert && panel.children[0] === shell, 'closing delayed shell shutdown or lost the outgoing frame');
    assert(ui.document.activeElement === ui.prompt && ui.splits[0].keyframes[0].flexBasis === '170px' &&
      ui.splits[0].keyframes[1].flexBasis === '0px', 'close did not start from the current split or restore focus');
    panel.rect.height = 95;
    const replacement = openTerminal(ui);
    await ui.idle();
    assert(ui.splits[0].cancelled && ui.splits[2].keyframes[0].flexBasis === '95px' &&
      panel.children[0] === replacement && !panel.inert, 'reopening snapped the split or lost the replacement');
    terminalEvent(ui, { type: 'softline-terminal-closed' }, shell.contentWindow);
    assert(panel.children[0] === replacement, 'late exit from the stopped shell closed its replacement');
    shortcut(ui, 'terminal');
    assert(!panel.children.length && !panel.hidden, 'closing before mount retained a frame that could still start a shell');
    ui.splits.slice(-2).forEach(animation => animation.finish()); await ui.idle();
    assert(panel.hidden && divider.hidden, 'finished close left the split in the layout');
  }, { motion: true });
  await check('animated transcript close retains the screen until it hides and can reverse into another tab', async ui => {
    transcript(ui, 'first'); transcript(ui, 'second');
    const panel = ui.get('transcripts'), toggle = ui.get('transcript-toggle');
    const [first, second] = ui.get('transcript-screen').children;
    toggle.click(); toggle.click();
    ui.get('transcript-close').click();
    assert(!panel.hidden && panel.inert && !second.hidden && toggle['aria-expanded'] === 'false', 'closing lost the outgoing screen or left it interactive');
    toggle.click(); await ui.idle();
    assert(ui.splits[0].cancelled && second.hidden && !first.hidden && !panel.inert, 'reopening exposed an old transcript or kept the pane inert');
    ui.get('transcript-close').click();
    ui.reduced.matches = true; ui.reduced.fire('change'); await ui.idle();
    assert(panel.hidden && ui.get('transcript-divider').hidden && first.hidden, 'enabling reduced motion did not finish closing');
    toggle.click(); ui.get('transcript-close').click();
    assert(panel.hidden && ui.splits.length === 6, 'reduced motion delayed closing a new split');
  }, { motion: true });
  await check('shell exit and host transcript removal animate independently in the same workspace', async ui => {
    openTerminal(ui);
    terminalEvent(ui, { type: 'softline-terminal-size', rowHeight: 18, chromeHeight: 40 });
    transcript(ui, 'log'); ui.get('transcript-toggle').click();
    const shell = ui.get('terminal-panel'), viewer = ui.get('transcripts');
    terminalEvent(ui, { type: 'softline-terminal-closed' });
    transcript(ui, 'log', 'remove');
    assert(shell.inert && viewer.inert && !shell.hidden && !viewer.hidden && ui.terminals[0].disposed, 'host cleanup or split exit was skipped');
    ui.splits.slice(2, 4).forEach(animation => animation.finish()); await ui.idle();
    assert(viewer.hidden && !shell.hidden, 'closing the transcript hid the shell before its own animation finished');
    ui.splits.slice(0, 2).forEach(animation => animation.finish()); await ui.idle();
    assert(shell.hidden && !shell.children.length, 'shell exit left a frame behind');
  }, { motion: true });
  await check('read-only transcripts retain independent geometry and do not interrupt chat', async ui => {
    const decode = bytes => Array.from(bytes, byte => String.fromCharCode(byte)).join('');
    assert(ui.get('transcript-toggle').hidden && !ui.window.fire('keydown', { key: 'F8' }).defaultPrevented, 'empty registry intercepted F8');
    transcript(ui, 'invalid', 'open', { rows: '201' });
    transcript(ui, 'log'); transcript(ui, 'pty', 'open', { rows: '40', columns: '140' });
    const [log, pty] = ui.terminals;
    const [logTab, ptyTab] = ui.get('transcript-tabs').children;
    assert(ui.terminals.length === 2 && log.options.disableStdin && pty.options.disableStdin && pty.osc.has(52) && pty.osc.has(8), 'viewer accepted input or unsafe side effects');
    transcript(ui, 'log', 'data', { text: 'line\n\x00next' });
    transcript(ui, 'pty', 'data', { encoding: 'base64', data: btoa('\x1b[?1049h') });
    assert(decode(log.chunks[0]) === 'line\r\nnext' && decode(pty.chunks[0]) === '\x1b[?1049h', 'text and VT routing lost bytes');
    ui.prompt.value = 'unsent draft';
    assert(ui.window.fire('keydown', { key: 'F8' }).defaultPrevented && logTab['aria-selected'] === 'true', 'F8 failed to open first transcript');
    assert(!ui.get('transcripts').hidden && ui.get('transcript-toggle')['aria-expanded'] === 'true', 'transcript panel did not open');
    ui.get('transcript-toggle').click();
    assert(ptyTab['aria-selected'] === 'true' && pty.options.cols === 140 && pty.options.rows === 40, 'cycling changed the source geometry');
    transcript(ui, 'pty', 'resize', { rows: '25', columns: '110' });
    assert(pty.options.cols === 110 && pty.options.rows === 25, 'producer resize was lost');
    ui.event({ type: 'delta', id: 'm', role: 'user', text: 'chat stayed live' });
    assert(ui.get('messages').children.at(-1).children[0].textContent === 'chat stayed live' && ui.prompt.value === 'unsent draft' && !ui.prompt.readOnly, 'transcript interrupted chat');
    ui.window.fire('keydown', { key: 'F8' });
    assert(ui.get('transcripts').hidden && !ui.get('transcript-toggle').hidden, 'cycling did not return to chat');
    transcript(ui, 'log', 'remove');
    assert(log.disposed, 'removed transcript retained its terminal');
    ui.event({ type: 'session', id: 'restarted' });
    assert(pty.disposed && ui.get('transcript-toggle').hidden, 'old transcript survived host restart');
  });
  await check('transcript tabs switch by mouse and keyboard and close the pane without interrupting output', async ui => {
    transcript(ui, 'log', 'open', { title: 'Run log' });
    transcript(ui, 'pty', 'open', { title: 'Worker screen' });
    const toggle = ui.get('transcript-toggle');
    const [log, pty] = ui.get('transcript-tabs').children;
    const [logPanel, ptyPanel] = ui.get('transcript-screen').children;
    toggle.click();
    assert(log.textContent === 'Run log' && pty.textContent === 'Worker screen', 'tabs did not use the host titles');
    assert(pty.role === 'tab' && pty['aria-controls'] === ptyPanel.id &&
      ptyPanel.role === 'tabpanel' && ptyPanel['aria-labelledby'] === pty.id, 'tab and panel were not associated');
    pty.click();
    assert(logPanel.hidden && !ptyPanel.hidden && log.tabIndex === -1 && pty.tabIndex === 0, 'click failed to switch the visible panel');
    pty.focus();
    assert(pty.fire('keydown', { key: 'ArrowRight' }).defaultPrevented &&
      ui.document.activeElement === log && log['aria-selected'] === 'true', 'arrow key failed to wrap to the first tab');
    log.fire('keydown', { key: 'End' });
    assert(ui.document.activeElement === pty && pty['aria-selected'] === 'true', 'End failed to select the last tab');
    ui.get('transcript-close').click();
    assert(ui.get('transcripts').hidden && toggle['aria-expanded'] === 'false' &&
      ui.document.activeElement === toggle, 'close failed to hide the pane and return focus');
    transcript(ui, 'pty', 'data', { text: 'still running' });
    const terminal = ui.terminals[1];
    toggle.click();
    pty.click();
    assert(!ptyPanel.hidden && terminal.chunks.length === 1 && !terminal.disposed, 'closing the pane interrupted the transcript');
  });
  await check('read-only transcript bounds pending bytes and recovers on host reset', async ui => {
    transcript(ui, 'log');
    ui.window.fire('keydown', { key: 'F8' });
    for (let i = 0; i < 17; i++) transcript(ui, 'log', 'data', { text: 'x'.repeat(16 * 1024) });
    const old = ui.terminals[0];
    assert(old.disposed && old.chunks.length === 16 && ui.get('transcript-screen').textContent.includes('fell behind'), 'bounded delivery failed');
    transcript(ui, 'log', 'reset');
    transcript(ui, 'log', 'data', { text: 'replayed' });
    const current = ui.terminals[1];
    assert(current.element && current.chunks.length === 1 &&
      ui.get('transcript-tabs').children[0]['aria-selected'] === 'true', 'reset did not restore the selected viewer');
  }, { stallTranscript: true });
  await check('docked terminal leaves prompt and conversation live', async ui => {
    const frame = openTerminal(ui);
    assert(frame && !ui.opened.length && !ui.prompt.readOnly && !ui.get('terminal-panel').hidden, 'terminal opening blocked prompt or opened another tab');
    assert(ui.document.activeElement === frame && frame.focusOptions.preventScroll, 'opening did not focus its terminal without scrolling');
    ui.prompt.focus();
    await ui.type('chat while shell runs'); ui.key('Enter'); await ui.idle();
    assert(ui.sent() === 'chat while shell runs' && ui.get('terminal-panel').children[0] === frame, 'sending interfered with shell');
    ui.event({ type: 'delta', id: 'live', role: 'user', text: 'feed continues' });
    assert(ui.get('messages').children.at(-1).children[0].textContent === 'feed continues', 'terminal paused the feed');
  });
  const paneSize = pane => pane.style.getPropertyValue('--pane-size');
  await check('opening the shell from the welcome screen moves the composer from its current position', async ui => {
    await ui.type('keep this draft');
    const frame = openTerminal(ui);
    await ui.idle();
    const panel = ui.get('terminal-panel');
    assert(panel.children[0] === frame && paneSize(panel) === '490px' && ui.prompt.value === 'keep this draft', 'welcome split changed the shell or draft');
    assert(ui.animations.length === 1 && ui.animations[0].keyframes[0].translate === '0 -290px', 'welcome composer snapped to the dock as the split opened');
  }, { motion: true, motionLayout: true });
  await check('terminal uses 25 measured rows or one third of the viewport without hiding the prompt', async ui => {
    openTerminal(ui);
    const panel = ui.get('terminal-panel');
    assert(paneSize(panel) === '490px', 'initial panel still reserves the old header');
    terminalEvent(ui, { type: 'softline-terminal-size', rowHeight: 17, chromeHeight: 58 });
    assert(paneSize(panel) === '483px', 'panel did not reserve 25 measured rows plus chrome');
    ui.window.innerHeight = 1800; ui.get('conversation-pane').rect.height = 1736; ui.window.fire('resize');
    assert(paneSize(panel) === '600px', 'large viewport did not reserve a third');
    ui.window.innerHeight = 420; ui.get('conversation-pane').rect.height = 356; ui.window.fire('resize');
    assert(parseFloat(paneSize(panel)) <= 165, 'short viewport hid the prompt');
    ui.get('composer-area').rect.height = 200; ui.resize(ui.get('composer-area'));
    assert(parseFloat(paneSize(panel)) <= 85, 'growing prompt overflowed the viewport');
  });
  await check('terminal stays below the prompt when the transcript stacks above chat', async ui => {
    openTerminal(ui);
    transcript(ui, 'log');
    ui.get('transcript-toggle').click();
    const conversation = ui.get('conversation-pane');
    conversation.rect = { top: 350, height: 240 };
    ui.resize(conversation);
    assert(paneSize(ui.get('terminal-panel')) === '64px', 'terminal sized itself against the whole viewport');
    assert(!ui.get('transcripts').hidden && !ui.prompt.readOnly, 'opening both panes disabled the prompt');
  });
  function pointer(handle, type, position) {
    return handle.fire(type, { isPrimary: true, button: 0, pointerId: 3, ...position });
  }
  await check('transcript divider adjusts the shared conversation and shell column and keeps separate mobile sizing', async ui => {
    const frame = openTerminal(ui);
    transcript(ui, 'log');
    ui.get('transcript-toggle').click();
    const handle = ui.get('transcript-divider'), panel = ui.get('transcripts');
    const workspace = ui.get('workspace'), shell = ui.get('terminal-panel');
    assert(!handle.hidden && handle['aria-orientation'] === 'vertical' && paneSize(panel) === '430px', 'desktop divider did not appear');
    assert(pointer(handle, 'pointerdown', { clientX: 586 }).defaultPrevented &&
      handle.hasPointerCapture(3) && workspace.dataset.paneResizing === 'x', 'drag did not capture across the shell');
    pointer(handle, 'pointermove', { clientX: 486 });
    assert(paneSize(panel) === '530px' && shell.children[0] === frame && paneSize(shell) === '490px', 'desktop drag changed the terminal session or height');
    pointer(handle, 'pointermove', { clientX: 0 });
    assert(paneSize(panel) === '696px', 'transcript could squeeze out the conversation');
    pointer(handle, 'pointerup', { clientX: 0 });
    handle.fire('keydown', { key: 'ArrowRight', shiftKey: true });
    assert(paneSize(panel) === '656px' && !workspace.dataset.paneResizing, 'keyboard resize or drag cleanup failed');
    ui.narrow.matches = true;
    ui.narrow.fire('change');
    const start = parseInt(paneSize(panel));
    assert(handle['aria-orientation'] === 'horizontal' && start === 267, 'mobile divider did not switch axes');
    pointer(handle, 'pointerdown', { clientY: 267 });
    pointer(handle, 'pointermove', { clientY: 313 });
    assert(paneSize(panel) === '313px', 'stacked divider did not grow the transcript downwards');
    ui.narrow.matches = false;
    ui.narrow.fire('change');
    assert(paneSize(panel) === '656px' && !handle.hasPointerCapture(3) && !workspace.dataset.paneResizing, 'axis change left a drag active or lost desktop size');
    ui.get('transcript-close').click();
    assert(handle.hidden && shell.children[0] === frame, 'closing the transcript hid the terminal or left its divider visible');
  });
  await check('terminal divider can shrink and grow the same shell and reserves room for the composer', async ui => {
    const frame = openTerminal(ui), panel = ui.get('terminal-panel'), handle = ui.get('terminal-divider');
    const workspace = ui.get('workspace');
    assert(!handle.hidden && handle['aria-orientation'] === 'horizontal', 'terminal did not show its divider');
    pointer(handle, 'pointerdown', { clientY: 278 });
    pointer(handle, 'pointermove', { clientY: 368 });
    pointer(handle, 'pointercancel', { clientY: 368 });
    assert(paneSize(panel) === '400px' && panel.children[0] === frame &&
      !workspace.dataset.paneResizing && !handle.hasPointerCapture(3), 'terminal drag replaced the shell or left the drag active');
    terminalEvent(ui, { type: 'softline-terminal-size', rowHeight: 17, chromeHeight: 58 });
    assert(paneSize(panel) === '400px', 'shell measurement overrode the dragged size');
    handle.fire('keydown', { key: 'ArrowUp' });
    assert(paneSize(panel) === '410px', 'Arrow Up did not grow the terminal');
    handle.fire('keydown', { key: 'End' });
    assert(paneSize(panel) === '496px', 'End did not use the largest size available');
    ui.get('composer-area').rect.height = 200;
    ui.resize(ui.get('composer-area'));
    assert(paneSize(panel) === '416px', 'resized terminal crowded out the composer');
    pointer(handle, 'pointerdown', { clientY: 278 });
    terminalEvent(ui, { type: 'softline-terminal-closed' });
    assert(handle.hidden && !workspace.dataset.paneResizing, 'shell exit leaked the divider or capture');
  });
  await check('response copy belongs to the message and returns the available Markdown throughout streaming', async ui => {
    ui.event({ type: 'begin', id: 'reply', role: 'assistant' });
    const message = ui.get('messages').children[0];
    const copy = message.children[1].children[0];
    assert(copy['aria-label'] === 'Copy response', 'begin did not create a response copy action');
    ui.event({ type: 'delta', id: 'reply', role: 'assistant', text: 'First **bold**\n' });
    copy.click(); await ui.idle();
    ui.event({ type: 'delta', id: 'reply', role: 'assistant', text: '\n`code`' });
    ui.event({ type: 'end', id: 'reply' });
    copy.click(); await ui.idle();
    assert(ui.copiedText[0] === 'First **bold**\n' &&
      ui.copiedText[1] === 'First **bold**\n\n`code`' && message.children[1].children.length === 1, 'copy depends on rendering or end duplicated the action');
  });
  await check('terminal messages validate source origin and bounded measurements', async ui => {
    const frame = openTerminal(ui), panel = ui.get('terminal-panel'), height = paneSize(panel);
    for (const data of [{ type: 'softline-terminal-size', rowHeight: Infinity, chromeHeight: 20 }, { type: 'softline-terminal-size', rowHeight: 0, chromeHeight: 20 }, { type: 'softline-terminal-size', rowHeight: 18, chromeHeight: -2 }]) terminalEvent(ui, data);
    terminalEvent(ui, { type: 'softline-terminal-closed' }, frame.contentWindow, 'http://other.example');
    terminalEvent(ui, { type: 'softline-terminal-closed' }, {});
    assert(panel.children[0] === frame && paneSize(panel) === height, 'untrusted event affected the panel');
    terminalEvent(ui, { type: 'softline-terminal-closed' });
    assert(ui.get('terminal-panel').hidden && !ui.get('terminal-panel').children.length && ui.document.activeElement === ui.prompt, 'shell exit did not remove the panel and return focus');
    assert(frame.contentWindow.messages.some(message => message.data.type === 'softline-terminal-stop'), 'closing did not request transport teardown');
  });
  await check('F9 from the live terminal toggles the toolbar without leaving the shell', async ui => {
    const frame = openTerminal(ui), toolbar = ui.get('toolbar');
    const toggle = { type: 'softline-toggle-toolbar' };
    terminalEvent(ui, toggle, {});
    terminalEvent(ui, toggle, frame.contentWindow, 'http://other.example');
    assert(!toolbar.hidden, 'untrusted terminal message changed the toolbar');
    terminalEvent(ui, toggle);
    assert(toolbar.hidden && ui.document.activeElement === frame && ui.get('terminal-panel').children[0] === frame, 'toolbar shortcut moved focus or closed the shell');
  });
  await check('terminal follows theme changes and closing invalidates stale frame messages', async ui => {
    const frame = openTerminal(ui); frame.fire('load');
    assert(frame.contentWindow.messages.at(-1).data.theme === 'default', 'initial panel theme not synchronized');
    ui.get('theme').value = 'monogreen'; ui.get('theme').fire('change');
    assert(frame.contentWindow.messages.at(-1).data.theme === 'monogreen', 'live theme did not reach terminal');
    shortcut(ui, 'terminal'); const replacement = openTerminal(ui);
    terminalEvent(ui, { type: 'softline-terminal-closed' }, frame.contentWindow);
    assert(ui.get('terminal-panel').children[0] === replacement, 'old shell exit closed its replacement');
  });
  await check('changing chat height follows the latest output only while follow is enabled', async ui => {
    openTerminal(ui);
    const scroll = ui.get('scroll'); scroll.scrollHeight = 1000; scroll.clientHeight = 250;
    ui.resize(scroll); assert(scroll.scrollTop === 1000, 'opening panel lost latest-output following');
    scroll.scrollTop = 100; scroll.fire('scroll');
    scroll.clientHeight = 200; ui.resize(scroll);
    assert(scroll.scrollTop === 100, 'resizing panel moved the reader away from older content');
  });
  await check('embedded chat uses its document base for mounted conversation and terminal routes', async ui => {
    assert(ui.socketURL().pathname === '/nested/events', 'conversation endpoint ignored document base');
    ui.event({ type: 'capabilities', terminal: '/nested/terminal' }); shortcut(ui, 'terminal');
    assert(ui.get('terminal-panel').children[0].src === 'http://localhost/nested/terminal', 'terminal ignored mounted route');
  }, { baseURI: 'http://localhost/nested/' });
  await check('host prompt lock preserves the draft and scrolling while blocking sends edits and new images', async ui => {
    await ui.type('keep this draft'); ui.prompt.setSelectionRange(2, 7);
    ui.event({ type: 'prompt_state', disabled: '1', text: 'Agent waiting for shell' });
    assert(ui.prompt.readOnly && ui.prompt.title === 'Agent waiting for shell' && ui.get('composer').dataset.disabled === '1' && ui.get('plus').disabled && ui.get('send').disabled, 'host lock did not dim/disable controls');
    ui.input('discard'); ui.key('Enter'); ui.form(); ui.key('Enter', { shiftKey: true }); ui.get('plus').click();
    ui.get('composer').fire('drop', { dataTransfer: { files: [{ name: 'ignored.png' }] } }); await ui.idle();
    assert(ui.prompt.value === 'keep this draft' && ui.prompt.selectionStart === 2 && ui.prompt.selectionEnd === 7 && !ui.sent() && ui.get('completions').hidden, 'locked prompt changed or submitted');
    assert(!ui.socket.sent.some(frame => frame.get('action') === 'image_begin'), 'locked prompt accepted an image');
    ui.get('scroll').focus(); ui.get('scroll').scrollTop = 10; ui.document.fire('keydown', { key: 'Escape' });
    assert(ui.document.activeElement === ui.get('scroll') && ui.get('scroll').scrollTop === 10, 'prompt lock interfered with scrolling/focus');
    ui.event({ type: 'delta', id: 'locked-feed', role: 'user', text: 'still live' });
    assert(ui.get('messages').children.at(-1).children[0].textContent === 'still live', 'lock stopped conversation updates');
    ui.event({ type: 'prompt_state', disabled: '0' }); ui.prompt.setSelectionRange(15, 15); ui.key('Enter'); await ui.idle();
    assert(ui.sent() === 'keep this draft' && !ui.prompt.readOnly && !ui.get('plus').disabled, 'host unlock did not restore input');
  }, { images: true });
  const terminalState = (ui, active, enabled) => ui.event({ type: 'terminal_state', active: String(active), disable_prompt: enabled ? '1' : '0' });
  await check('terminal policy defaults to concurrent input and can change while shells run', async ui => {
    await ui.type('independent draft');
    terminalState(ui, 2, false); assert(!ui.prompt.readOnly, 'default terminal policy disabled input');
    terminalState(ui, 2, true); assert(ui.prompt.readOnly && ui.get('terminal-panel').hidden, 'another view handoff did not lock this prompt');
    terminalState(ui, 2, false); assert(!ui.prompt.readOnly && ui.prompt.value === 'independent draft', 'live policy change failed to restore the draft');
    terminalState(ui, 2, true); assert(ui.prompt.readOnly, 'live policy change did not relock');
    terminalState(ui, 1, true); assert(ui.prompt.readOnly, 'first shell exit unlocked another active handoff');
    terminalState(ui, 0, true); assert(!ui.prompt.readOnly, 'last shell exit retained automatic lock');
  });
  await check('terminal policy and explicit host locks remain independent', async ui => {
    ui.event({ type: 'prompt_state', disabled: '1', text: 'Host maintenance' });
    terminalState(ui, 1, true); terminalState(ui, 0, true);
    assert(ui.prompt.readOnly && ui.prompt.title === 'Host maintenance', 'shell exit cleared explicit lock');
    terminalState(ui, 1, true); ui.event({ type: 'prompt_state', disabled: '0' });
    assert(ui.prompt.readOnly, 'clearing explicit lock overrode terminal policy');
    terminalState(ui, 1, false); assert(!ui.prompt.readOnly, 'disabling automatic policy retained its lock');
  });
  await check('unregistered slash and at-sign input stays literal and has no candidates', async ui => {
    await ui.type('/custom @path'); await ui.idle();
    assert(!ui.queries().length && !ui.ghost(), 'unregistered text triggered completion');
    ui.get('plus').click(); ui.reply(ui.queries()[0], []);
    assert(ui.get('completions').children.every(item => !/^[/@]/u.test(item.children[1].textContent)), 'menu invented a command');
    ui.form(); await ui.idle();
    assert(ui.sent() === '/custom @path', 'unregistered input was interpreted locally');
    terminalState(ui, 1, true);
    assert(ui.get('plus').disabled, 'UI supplied a built-in unlock command');
  }, { manual: true, noProviders: true });
  await check('reconnect drops trigger registrations the host no longer publishes', async ui => {
    await ui.type('@l'); await ui.idle();
    assert(ui.ghost(), 'initial host registration did not provide a candidate');
    ui.socket.close(); ui.timeout(1000);
    ui.event({ type: 'session', id: 'initial' }); await ui.idle();
    assert(!ui.queries().length && !ui.ghost(), 'reconnect reused the old registration');
    ui.form(); await ui.idle();
    assert(ui.sent() === '@l', 'removed registration stripped the at-sign');
    await ui.type('/ap'); await ui.idle();
    assert(!ui.queries().length && !ui.ghost(), 'reconnect reused the old slash registration');
  });
  await check('the locked menu submits a host command verbatim and preserves the draft', async ui => {
    const command = 'resume-composer allow';
    await ui.type('keep this draft'); ui.prompt.setSelectionRange(2, 7);
    terminalState(ui, 1, true);
    ui.event({ type: 'capabilities', terminal_unlock_command: 'resume\ncomposer' });
    assert(ui.get('plus').disabled, 'menu accepted a multiline command');
    ui.event({ type: 'capabilities', terminal_unlock_command: command });
    assert(ui.prompt.readOnly && !ui.get('plus').disabled && ui.get('send').disabled, 'locked prompt stranded its own policy control');
    ui.get('plus').click();
    const items = ui.get('completions').children;
    assert(items.length === 1 && items[0].children[1].textContent === command, 'locked menu did not show the host command');
    assert(ui.document.activeElement === items[0], 'keyboard focus did not enter the locked menu');
    items[0].click();
    assert(ui.sent() === command && ui.prompt.readOnly, 'menu did not send the host text or wait for its policy');
    terminalState(ui, 1, false);
    assert(!ui.prompt.readOnly && ui.prompt.value === 'keep this draft' && ui.prompt.selectionStart === 2 && ui.prompt.selectionEnd === 7, 'policy command lost the draft or selection');
    terminalState(ui, 1, true); ui.event({ type: 'prompt_state', disabled: '1' });
    assert(ui.get('plus').disabled, 'terminal command bypassed an explicit host lock');
  }, { images: true });
  await check('closing a browser panel cannot unlock a handoff in another view', async ui => {
    openTerminal(ui); terminalState(ui, 2, true);
    terminalEvent(ui, { type: 'softline-terminal-closed' });
    assert(ui.prompt.readOnly, 'panel closure bypassed server state');
    terminalState(ui, 1, true); assert(ui.prompt.readOnly, 'remaining native handoff lost its lock');
    terminalState(ui, 0, true); assert(!ui.prompt.readOnly && ui.document.activeElement === ui.prompt, 'last handoff failed to restore prompt');
  });
  await check('late join and same-host reconnect use the shared terminal snapshot', async ui => {
    terminalState(ui, 3, true);
    ui.socket.close(); ui.timeout(1000); ui.event({ type: 'session', id: 'initial' });
    assert(ui.prompt.readOnly, 'same-host reconnect cleared active handoffs');
    terminalState(ui, 0, true); assert(!ui.prompt.readOnly, 'current replay snapshot did not unlock');
    terminalState(ui, 1, true); ui.event({ type: 'session', id: 'replacement-host' });
    assert(!ui.prompt.readOnly, 'new host inherited old active handoffs/policy');
    terminalState(ui, 0, false); assert(!ui.prompt.readOnly, 'new host default was not concurrent');
  });
  await check('invalid terminal snapshots cannot bypass the automatic lock', async ui => {
    terminalState(ui, 1, true);
    for (const fields of [{ active: '-1', disable_prompt: '1' }, { active: '0oops', disable_prompt: '0' }, { active: '9007199254740992', disable_prompt: '0' }, { active: '0' }, { active: '0', disable_prompt: 'false' }]) {
      ui.event({ type: 'terminal_state', ...fields }); assert(ui.prompt.readOnly, 'invalid state cleared lock');
    }
    ui.event({ type: 'terminal_state', active: '0', disable_prompt: '0', view: 'different-view' });
    assert(ui.prompt.readOnly, 'another view override cleared lock');
    terminalState(ui, 0, true); assert(!ui.prompt.readOnly, 'valid final snapshot did not unlock');
  });
  await check('host lock invalidates pending completion edits across an unlock', async ui => {
    ui.input('@R'); ui.key('Tab'); ui.key(' ');
    const pending = ui.queries()[0];
    ui.event({ type: 'prompt_state', disabled: '1' });
    ui.event({ type: 'prompt_state', disabled: '0' });
    ui.reply(pending); await ui.idle();
    assert(ui.prompt.value === '@R', 'late completion or queued input changed the suspended draft');
  }, { manual: true });
  for (const automatic of [false, true]) await check((automatic ? 'automatic' : 'explicit') + ' lock retains literal filename completion metadata', async ui => {
    ui.get('plus').click(); await ui.idle();
    ui.event({ type: 'complete', request: ui.queries().at(-1).get('request'), label: '@literal.txt', value: '@literal.txt', start: '0', end: '0', continue: '0' });
    ui.get('completions').children.find(item => item.textContent.includes('@literal.txt')).click(); await ui.idle();
    assert(ui.prompt.value === '@literal.txt', 'literal filename fixture did not insert');
    if (automatic) {
      ui.event({ type: 'capabilities', terminal_unlock_command: '/resume-composer' });
      terminalState(ui, 1, true);
      ui.get('plus').click(); ui.get('completions').children[0].click();
      terminalState(ui, 1, false);
    } else {
      ui.event({ type: 'prompt_state', disabled: '1' });
      ui.event({ type: 'prompt_state', disabled: '0' });
    }
    await ui.idle();
    ui.form(); await ui.idle();
    assert(ui.sent() === '@literal.txt', 'lock/unlock turned a literal filename into completion syntax');
  });
  await check('unlock after shell exit leaves subsequently focused controls alone', async ui => {
    openTerminal(ui); ui.event({ type: 'prompt_state', disabled: '1' });
    terminalEvent(ui, { type: 'softline-terminal-closed' });
    ui.get('font').focus(); ui.event({ type: 'prompt_state', disabled: '0' });
    assert(ui.document.activeElement === ui.get('font'), 'delayed host unlock stole user-selected focus');
  });
  await check('only the host unlocks a terminal-paused prompt and a new host clears stale locks', async ui => {
    const frame = openTerminal(ui);
    ui.event({ type: 'prompt_state', disabled: '1' });
    terminalEvent(ui, { type: 'softline-terminal-closed' });
    assert(ui.prompt.readOnly && ui.get('terminal-panel').hidden && ui.document.activeElement === ui.get('scroll'), 'shell exit overrode the host lock');
    ui.event({ type: 'prompt_state', disabled: '0' });
    assert(ui.document.activeElement === ui.prompt, 'unlock after terminal exit did not restore prompt focus');
    ui.event({ type: 'prompt_state', disabled: '1' });
    openTerminal(ui); ui.event({ type: 'session', id: 'new-host' });
    assert(ui.get('terminal-panel').hidden && !ui.prompt.readOnly && !ui.get('plus').disabled, 'host restart retained old terminal or lock');
    terminalEvent(ui, { type: 'softline-terminal-closed' }, frame.contentWindow);
    ui.event({ type: 'prompt_state', disabled: '1', view: 'another-browser' });
    assert(!ui.prompt.readOnly, 'another view locked this prompt');
  });
  await check('Escape closes the plus menu and its focus action restores the prompt', async ui => {
    ui.get('plus').focus(); ui.get('plus').click(); await ui.idle();
    const item = ui.get('completions').children.find(item => item.textContent.includes('Focus prompt'));
    assert(item?.textContent.includes('Esc'), 'focus action or shortcut hint missing');
    item.focus(); item.click();
    assert(ui.document.activeElement === ui.prompt && ui.get('completions').hidden, 'focus action did not close the menu and restore focus');
    ui.get('plus').focus(); ui.get('plus').click(); await ui.idle();
    const event = ui.document.fire('keydown', { target: ui.get('plus'), key: 'Escape' });
    assert(event.defaultPrevented && ui.document.activeElement === ui.prompt && ui.get('completions').hidden, 'Escape left the plus menu focused/open');
  });
  await check('sending through the form returns focus to the prompt', async ui => {
    await ui.type('send this'); ui.get('send').focus(); ui.form(); await ui.idle();
    assert(ui.sent() === 'send this' && !ui.prompt.value && ui.document.activeElement === ui.prompt, 'successful send did not return focus');
  });
  await check('returning focus stays immediate while a completion provider is pending', async ui => {
    ui.input('@R'); ui.key('Tab'); ui.get('scroll').focus();
    const event = ui.document.fire('keydown', { target: ui.get('scroll'), key: 'Escape' });
    assert(event.defaultPrevented && ui.document.activeElement === ui.prompt && ui.prompt.value === '@R', 'pending completion delayed focus or changed the draft');
    ui.reply(ui.queries()[0]); await ui.idle();
    assert(ui.document.activeElement === ui.prompt, 'late completion displaced prompt focus');
  }, { manual: true });
  await check('native navigation collapses selections and preserves platform modifiers', async ui => {
    await ui.type('abcdefghij');
    ui.prompt.setSelectionRange(2, 7);
    assert(!ui.key('ArrowRight').defaultPrevented && ui.prompt.selectionStart === 7, 'Right did not use native selection collapse');
    ui.prompt.setSelectionRange(2, 7);
    assert(!ui.key('ArrowLeft').defaultPrevented && ui.prompt.selectionStart === 2, 'Left did not use native selection collapse');
    assert(!ui.key('ArrowLeft', { metaKey: true }).defaultPrevented && ui.prompt.selectionStart === 0, 'Cmd+Left was intercepted');
    assert(!ui.key('ArrowRight', { ctrlKey: true }).defaultPrevented, 'native word navigation was intercepted');
    assert(!ui.key('ArrowRight', { shiftKey: true }).defaultPrevented, 'native selection extension was intercepted');
  });
  for (const type of ['deleteByCut', 'deleteWordBackward', 'historyUndo']) await check(type + ' remains native during pending completion', async ui => {
    ui.input('@l'); ui.key('Tab');
    ui.prompt.setSelectionRange(0, 2);
    const event = ui.prompt.fire('beforeinput', { inputType: type, cancelable: true });
    assert(!event.defaultPrevented, 'unreplayable edit was swallowed');
    ui.prompt.setRangeText('', 0, 2, 'end'); ui.prompt.fire('input', { inputType: type });
    ui.reply(ui.queries()[0]); await ui.idle();
    assert(ui.prompt.value === '' && !ui.ghost(), 'late completion overwrote native deletion');
  }, { manual: true });
  await check('approval shortcuts require focus, retain FIFO order and ignore repeat', async ui => {
    queueState(ui, [{ id: 'a', revision: '1' }, { id: 'b', revision: '2' }]);
    shortcut(ui, 'approve');
    assert(!ui.socket.sent.some(frame => frame.get('action') === 'approve'), 'unfocused request was approved');
    shortcut(ui, 'approvals');
    assert(ui.document.activeElement?.dataset.id === 'a', 'oldest approval did not receive focus');
    shortcut(ui, 'approve', { repeat: true });
    assert(!ui.socket.sent.some(frame => frame.get('action') === 'approve'), 'key repeat approved a request');
    shortcut(ui, 'approve');
    const approved = ui.socket.sent.filter(frame => frame.get('action') === 'approve');
    assert(approved.length === 1 && approved[0].get('id') === 'a' && approved[0].get('revision') === '1', 'approval lost its identity/revision');
    shortcut(ui, 'reject');
    assert(!ui.socket.sent.some(frame => frame.get('action') === 'reject'), 'decision retained focus for another action');
    shortcut(ui, 'approvals'); ui.key('Escape'); shortcut(ui, 'reject');
    assert(!ui.socket.sent.some(frame => frame.get('action') === 'reject'), 'Escape retained approval focus');
    shortcut(ui, 'approvals'); queueState(ui, [{ id: 'a', revision: '3' }]); shortcut(ui, 'approve');
    assert(ui.socket.sent.filter(frame => frame.get('action') === 'approve').length === 1, 'stale focus approved a changed request');
  });
  await check('approval focus survives unrelated queue redraws and returns to prompt after invalidation', async ui => {
    queueState(ui, [{ id: 'a', revision: '1' }]); shortcut(ui, 'approvals');
    queueState(ui, [{ id: 'a', revision: '1' }, { id: 'b', revision: '2' }]);
    assert(ui.document.activeElement === ui.get('queue').children[0], 'redraw left keyboard focus on a detached row');
    queueState(ui, [{ id: 'a', revision: 'changed' }]);
    assert(ui.document.activeElement === ui.prompt, 'changed approval retained unsafe focus');
  });
  await check('Ctrl+Right remains native with an active completion', async ui => {
    await ui.type('@l');
    assert(!ui.key('ArrowRight', { ctrlKey: true }).defaultPrevented, 'word navigation accepted a ghost');
    await ui.idle(); assert(ui.prompt.value === '@l', 'word navigation inserted a completion');
  });
  await check('paste without replayable text stays native during pending completion', async ui => {
    ui.input('@l'); ui.key('Tab');
    const before = ui.prompt.fire('beforeinput', { inputType: 'insertFromPaste', data: null, cancelable: true });
    assert(!before.defaultPrevented, 'paste was cancelled without available text');
    ui.prompt.setRangeText(' pasted', ui.prompt.selectionStart, ui.prompt.selectionEnd, 'end');
    ui.prompt.fire('input', { inputType: 'insertFromPaste' });
    ui.reply(ui.queries()[0]); await ui.idle();
    assert(ui.prompt.value === 'l pasted', 'pending completion lost native pasted text');
  }, { manual: true });
  await check('approval buttons are Approve Edit Reject and edit preserves the independent draft', async ui => {
    ui.event({ type: 'capabilities', edit: '1' });
    await ui.type('independent unsent draft');
    queueState(ui, [{ id: 'a', revision: '1' }, { id: 'b', revision: '2' }]);
    const rows = ui.get('queue').children;
    assert(rows[0].children.slice(2).map(button => button.textContent).join(',') === 'Approve,Edit,Reject', 'approval actions have the wrong order');
    assert(rows[1].children.slice(2).every(button => button.disabled), 'later approval actions bypass FIFO');
    rows[0].children[3].click();
    assert(ui.get('queue-editor').open && ui.get('queue-edit-text').value === 'Approval text', 'Edit did not open the separate editor');
    ui.get('queue-edit-text').value = 'changed approval'; ui.get('queue-edit-form').fire('submit');
    const request = ui.socket.sent.filter(frame => frame.get('action') === 'edit').at(-1);
    assert(request?.get('id') === 'a' && request.get('revision') === '1' && request.get('text') === 'changed approval', 'edit lost queue identity/revision');
    assert(ui.prompt.value === 'independent unsent draft', 'queue edit overwrote prompt');
    ui.event({ type: 'error', request: request.get('request'), text: 'Entry changed' });
    assert(ui.get('queue-editor').open && ui.get('queue-edit-text').value === 'changed approval' && !ui.get('queue-edit-save').disabled, 'conflict discarded edit or prevented retry');
    ui.get('queue-edit-form').fire('submit');
    const retry = ui.socket.sent.filter(frame => frame.get('action') === 'edit').at(-1);
    ui.event({ type: 'accepted', request: retry.get('request') });
    assert(!ui.get('queue-editor').open && ui.prompt.value === 'independent unsent draft', 'successful edit lost independent draft');
  });
  await check('Enter closes an unchanged approval edit without submitting a queue mutation', async ui => {
    ui.event({ type: 'capabilities', edit: '1' }); await ui.type('independent draft');
    queueState(ui, [{ id: 'a', revision: '1' }]); shortcut(ui, 'edit');
    // Other queue metadata can change while this harmless, unchanged form is open.
    queueState(ui, [{ id: 'a', revision: '2' }]);
    const enter = ui.get('queue-edit-text').fire('keydown', { key: 'Enter' });
    assert(enter.defaultPrevented && !ui.get('queue-editor').open, 'Enter did not finish an unchanged edit');
    assert(!ui.socket.sent.some(frame => frame.get('action') === 'edit') && ui.prompt.value === 'independent draft', 'unchanged edit mutated queue or draft');
  });
  await check('Enter saves changed approval text, while Shift+Enter and composition stay native', async ui => {
    ui.event({ type: 'capabilities', edit: '1' });
    queueState(ui, [{ id: 'a', revision: '1' }]); shortcut(ui, 'edit');
    const input = ui.get('queue-edit-text'); input.value = 'updated approval';
    assert(!input.fire('keydown', { key: 'Enter', shiftKey: true }).defaultPrevented, 'Shift+Enter could not insert a newline');
    assert(!input.fire('keydown', { key: 'Enter', isComposing: true }).defaultPrevented, 'composition submitted an edit');
    input.fire('keydown', { key: 'Enter' });
    const edit = ui.socket.sent.find(frame => frame.get('action') === 'edit');
    assert(edit?.get('text') === 'updated approval' && !edit.get('original') && ui.get('queue-editor').open, 'Enter did not submit the edited text or wait for admission');
    ui.event({ type: 'accepted', request: edit.get('request') });
    assert(!ui.get('queue-editor').open, 'accepted edit left the form open');
  });
  await check('plus actions expose shortcuts and terminal is enabled only by the host', async ui => {
    ui.get('plus').click(); await ui.idle();
    let items = ui.get('completions').children;
    assert(items.some(item => item.textContent.includes('Focus approvals') && item.textContent.includes('Ctrl+Shift+1')), 'approval shortcut is undiscoverable');
    assert(items.find(item => item.textContent.includes('Open terminal')).disabled, 'unavailable terminal was enabled');
    shortcut(ui, 'terminal'); assert(ui.get('terminal-panel').hidden, 'terminal launched without host support');
    ui.event({ type: 'capabilities', edit: '1', terminal: '/terminal' });
    shortcut(ui, 'terminal'); assert(!ui.get('terminal-panel').hidden && ui.get('terminal-panel').children[0]?.src === 'http://localhost/terminal' && !ui.opened.length, 'terminal did not dock the host endpoint');
    shortcut(ui, 'terminal'); assert(ui.get('terminal-panel').hidden, 'terminal shortcut did not close its panel');
    queueState(ui, [{ id: 'a', revision: '1', held: '0' }]);
    shortcut(ui, 'edit'); assert(ui.get('queue-editor').open, 'edit shortcut did not open newest entry');
    ui.get('queue-edit-cancel').click();
    ui.get('plus').click(); await ui.idle();
    ui.get('completions').children.find(item => item.textContent.includes('Edit queued message')).click();
    assert(ui.get('queue-editor').open, '+ action and keyboard binding differ');
  });
  await check('approval menu actions and reject shortcut use the same focused identity', async ui => {
    queueState(ui, [{ id: 'a', revision: '1' }]);
    ui.get('plus').click(); await ui.idle();
    ui.get('completions').children.find(item => item.textContent.includes('Focus approvals')).click();
    ui.get('plus').click(); await ui.idle();
    ui.get('completions').children.find(item => item.textContent.includes('Approve focused request')).click();
    assert(ui.socket.sent.some(frame => frame.get('action') === 'approve' && frame.get('id') === 'a'), 'menu approval did not dispatch');
    shortcut(ui, 'approvals'); shortcut(ui, 'reject');
    assert(ui.socket.sent.some(frame => frame.get('action') === 'reject' && frame.get('id') === 'a' && frame.get('revision') === '1'), 'reject shortcut lost focused identity');
  });
  await check('approval actions do not wait for completion and survive provider failure', async ui => {
    queueState(ui, [{ id: 'a', revision: '1' }]);
    ui.input('@l'); ui.key('Tab'); ui.get('plus').click();
    ui.get('completions').children.find(item => item.textContent.includes('Focus approvals')).click();
    shortcut(ui, 'approve');
    assert(ui.socket.sent.some(frame => frame.get('action') === 'approve'), 'completion blocked approval');
    ui.get('plus').click();
    const menu = ui.queries().findLast(query => query.get('all') === '1');
    ui.reply(menu, [], 'error');
    assert(!ui.get('completions').hidden && ui.get('completions').textContent.includes('Focus approvals'), 'provider failure hid host actions');
    ui.reply(ui.queries()[0]); await ui.idle();
  }, { manual: true });
  await check('terminal host paths cannot open an external origin', async ui => {
    for (const terminal of ['//external.test/terminal', '/\\external.test/terminal', 'https://external.test/terminal', '/terminal\n']) {
      ui.event({ type: 'capabilities', terminal }); shortcut(ui, 'terminal');
    }
    assert(!ui.opened.length, 'unvalidated terminal path opened');
  });
  await check('reconnect actions wait for session synchronization', async ui => {
    await ui.type('draft'); ui.socket.close(); ui.timeout(1000);
    ui.key('Enter'); await ui.idle();
    assert(ui.prompt.value === 'draft', 'unsynchronized connection consumed draft');
    ui.event({ type: 'session', id: 'initial' }); ui.key('Enter'); await ui.idle();
    assert(ui.sent() === 'draft', 'synchronized connection could not submit');
  });
  await check('same-epoch reconnect retries by default with the original request identity', async ui => {
    await ui.type('send once'); ui.key('Enter'); await ui.idle();
    const sent = ui.socket.sent.find(frame => frame.get('action') === 'send');
    ui.socket.close(); ui.timeout(1000); ui.event({ type: 'session', id: 'initial' });
    const retry = ui.socket.sent.find(frame => frame.get('action') === 'send');
    assert(retry?.toString() === sent?.toString() && retry, 'default retry changed or lost the original request');
    ui.event({ type: 'accepted', request: sent.get('request') });
    ui.socket.close(); ui.timeout(1000); ui.event({ type: 'session', id: 'initial' });
    assert(!ui.sent(), 'confirmed request was retried');
  });
  await check('the reconnecting host can park prompts, steers and edits as delivery unconfirmed', async ui => {
    await ui.type('Run the tests.'); ui.key('Enter'); await ui.idle();
    await ui.type('Use the debug build.'); ui.key('Enter', { altKey: true }); await ui.idle();
    await ui.type('Independent draft');
    ui.event({ type: 'capabilities', edit: '1' });
    queueState(ui, [{ id: 'a', revision: '1' }]); shortcut(ui, 'edit');
    ui.get('queue-edit-text').value = 'Updated approval'; ui.get('queue-edit-form').fire('submit');
    const original = ui.socket.sent.filter(frame => ['send', 'steer', 'edit'].includes(frame.get('action')));
    assert(original.map(frame => frame.get('action')).join(',') === 'send,steer,edit', 'fixture did not send all three actions');
    ui.socket.close(); ui.timeout(1000);
    ui.event({ type: 'session', id: 'initial', disable_auto_resend: '1' });
    assert(!ui.socket.sent.length, 'a potentially accepted action was automatically resent');
    assert(!ui.get('queue-editor').open && ui.prompt.value === 'Independent draft', 'reconnect blocked recovery or overwrote the draft');
    ui.socket.close(); ui.timeout(1000); ui.event({ type: 'session', id: 'initial' });
    assert(!ui.socket.sent.length, 'a later reconnect retried a recovered action');
    ui.get('plus').click(); await ui.idle();
    const recovery = () => ui.get('completions').children.filter(item => item.textContent.includes('Delivery unconfirmed.'));
    assert(recovery().length === 3, 'unconfirmed requests are missing from recovery');
    ui.event({ type: 'accepted', request: original[0].get('request') });
    assert(recovery().length === 2, 'a late receipt left a confirmed prompt in recovery');
    ui.event({ type: 'error', request: original[1].get('request'), text: 'Steer rejected' });
    assert(recovery().length === 1 && ui.get('completions').textContent.includes('Recover unsent prompt'), 'a late rejection did not retain the text as a failed request');
    assert(recovery()[0].textContent.includes('Updated approval'), 'recovery lost the edited text');
    recovery()[0].click(); await ui.idle();
    assert(ui.prompt.value === 'Independent draft\nUpdated approval', 'manual recovery did not preserve both texts');
    assert(!ui.socket.sent.some(frame => ['send', 'steer', 'edit'].includes(frame.get('action'))), 'manual recovery submitted an action');
  });
  await check('a host epoch change preserves a full batch of unconfirmed requests', async ui => {
    for (let i = 0; i < 32; ++i) {
      ui.input(`Prompt number ${i}.`); ui.key('Enter'); await ui.idle();
    }
    const original = ui.socket.sent.filter(frame => frame.get('action') === 'send');
    assert(original.length === 32, 'fixture did not fill the pending request window');
    ui.event({ type: 'capabilities', edit: '1' });
    queueState(ui, [{ id: 'a', revision: '1' }]); shortcut(ui, 'edit');
    ui.get('queue-edit-text').value = 'Keep this edit'; ui.get('queue-edit-form').fire('submit');
    assert(!ui.socket.sent.some(frame => frame.get('action') === 'edit') && ui.get('queue-edit-text').value === 'Keep this edit' &&
      ui.get('queue-edit-error').textContent.includes('pending requests'), 'a full pending window did not leave the unsent edit available');
    ui.get('queue-edit-cancel').click();
    ui.socket.close(); ui.timeout(1000); ui.event({ type: 'session', id: 'new' });
    assert(!ui.socket.sent.length, 'a request crossed into a new epoch');
    ui.get('plus').click(); await ui.idle();
    const recovery = () => ui.get('completions').children.filter(item => item.textContent.includes('Delivery unconfirmed.'));
    assert(recovery().length === 32, 'recovery dropped part of the pending batch');
    ui.event({ type: 'accepted', request: original[0].get('request') });
    assert(recovery().length === 32, 'a new epoch changed the outcome of an old request');
  });
  await check('host epoch changes reset conversation IDs, queue and reasoning while retaining the draft', async ui => {
    ui.event({ type: 'session', id: 'old' }); await ui.type('keep this draft');
    ui.event({ type: 'delta', id: 'm1', role: 'user', text: 'old conversation', seq: 'old.20' });
    queueState(ui, [{ id: 'old-approval', revision: '1' }]);
    ui.event({ type: 'status', busy: '1', text: 'old reasoning' });
    ui.socket.close(); ui.timeout(1000);
    assert(ui.socketURL().searchParams.get('after') === 'old.20', 'reconnect lost epoch-bound cursor');
    ui.event({ type: 'session', id: 'new' });
    assert(!ui.get('messages').children.length && ui.get('queue').hidden && !ui.get('activity').textContent, 'new epoch retained old conversation state');
    ui.event({ type: 'delta', id: 'm1', role: 'user', text: 'new conversation' });
    assert(ui.get('messages').textContent === 'new conversation' && ui.prompt.value === 'keep this draft', 'reused message ID merged sessions or draft was lost');
  });
  await check('host restart invalidates an open edit target even when queue IDs are reused', async ui => {
    ui.event({ type: 'capabilities', edit: '1' });
    queueState(ui, [{ id: 'a', revision: '1' }]); shortcut(ui, 'edit');
    ui.get('queue-edit-text').value = 'keep this edited text';
    ui.event({ type: 'session', id: 'restarted' });
    ui.event({ type: 'capabilities', edit: '1' });
    queueState(ui, [{ id: 'a', revision: '1', text: 'unrelated new request' }]);
    ui.get('queue-edit-form').fire('submit');
    assert(!ui.socket.sent.some(frame => frame.get('action') === 'edit'), 'old edit was applied to a new conversation');
    assert(ui.get('queue-editor').open && ui.get('queue-edit-text').value === 'keep this edited text' && ui.get('queue-edit-save').disabled, 'restart discarded edit text or left stale saving enabled');
    ui.get('queue-edit-cancel').click(); shortcut(ui, 'edit');
    assert(ui.get('queue-edit-text').value === 'unrelated new request' && !ui.get('queue-edit-save').disabled, 'new session could not start a deliberate edit');
  });
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
  await check('toolbar branding is hidden and empty by default', async ui => {
    assert(ui.get('brand').hidden && ui.get('brand').innerHTML === '', 'default toolbar included branding');
  });
  const header = '<a href="./"><svg aria-label="My project"></svg><img src="./logo.svg" alt="Logo"></a>';
  await check('trusted custom header HTML is visible independently of welcome branding', async ui => {
    assert(!ui.get('brand').hidden && ui.get('brand').innerHTML === header, 'host header HTML was lost or hidden');
    assert(ui.get('banner').innerHTML === '<b>Welcome banner</b>', 'header replaced the welcome banner');
  }, { config: { header, banner: '<b>Welcome banner</b>' } });
  await check('whitespace header does not reserve a toolbar branding slot', async ui => {
    assert(ui.get('brand').hidden, 'empty host branding took toolbar space');
  }, { config: { header: ' \n\t' } });
  const imagePath = '/backend/softline-image-€ image.png';
  await check('acknowledged image keeps its local preview while only the path is sent', async ui => {
    const { url } = await ui.upload(imagePath);
    assert(ui.prompt.value === imagePath && ui.get('uploads').hidden, 'completed upload did not leave just the path');
    assert(!ui.revoked.includes(url) && ui.get('image-preview').hidden, 'preview was revoked or shown permanently');
    const span = ui.get('projection-before').children.find(child => child.dataset?.imagePath);
    assert(span?.textContent === imagePath, 'draft path did not retain a preview reference');
    span.rects = [{ left: 10, top: 4, right: 200, bottom: 24 }];
    ui.prompt.fire('pointermove', { clientX: 20, clientY: 10 });
    assert(!ui.get('image-preview').hidden && ui.get('image-preview-content').src === url, 'hover did not show local upload');
    ui.prompt.fire('pointerleave'); ui.timeout(150); assert(ui.get('image-preview').hidden, 'preview remained after leaving path');
    ui.form(); await ui.idle(); assert(ui.sent() === imagePath, 'preview changed the actual submitted prompt');
  }, { images: true });
  await check('a wrapped draft path previews only inside the visible editor', async ui => {
    await ui.upload(imagePath);
    const span = ui.get('projection-before').children.find(child => child.dataset?.imagePath);
    span.rects = [{ left: 350, top: 4, right: 400, bottom: 24 }, { left: 0, top: 24, right: 200, bottom: 44 }];
    ui.prompt.fire('pointermove', { clientX: 20, clientY: 30 });
    assert(!ui.get('image-preview').hidden, 'second wrapped path segment did not preview');
    ui.prompt.fire('pointermove', { clientX: 20, clientY: 40 });
    assert(ui.get('image-preview').hidden, 'offscreen path segment opened a preview');
    ui.prompt.fire('pointermove', { clientX: 360, clientY: 10, buttons: 1 });
    assert(ui.get('image-preview').hidden, 'dragging a text selection opened a preview');
  }, { images: true });
  await check('editing the final path invalidates its draft preview without changing text', async ui => {
    await ui.upload(imagePath);
    ui.prompt.setSelectionRange(ui.prompt.value.length, ui.prompt.value.length); ui.input('x');
    assert(!ui.get('projection-before').children.some(child => child.dataset?.imagePath), 'edited filename retained a stale reference');
    ui.form(); await ui.idle(); assert(ui.sent() === imagePath + 'x', 'preview normalization rewrote edited path');
  }, { images: true });
  await check('streamed user paths link across every possible chunk boundary', async ui => {
    const { url } = await ui.upload(imagePath);
    for (let cut = 1; cut < imagePath.length; ++cut) {
      const id = 'image-cut-' + cut, source = 'See (' + imagePath + ') please';
      ui.event({ type: 'begin', id, role: 'user' });
      ui.event({ type: 'delta', id, role: 'user', text: 'See (' + imagePath.slice(0, cut) });
      const body = ui.get('messages').children.at(-1).children[0];
      assert(body.textContent === 'See (' + imagePath.slice(0, cut), 'unfinished path bytes were hidden');
      ui.event({ type: 'delta', id, role: 'user', text: imagePath.slice(cut) + ') please' });
      ui.event({ type: 'end', id });
      assert(body.textContent === source, 'path decoration changed streamed text');
      const reference = body.children.find(child => child.dataset?.imagePath);
      assert(reference?.textContent === imagePath, 'split path was not recognized');
      reference.fire('focus');
      assert(ui.get('image-preview-content').src === url && !ui.get('image-preview').hidden, 'keyboard focus lost the local preview');
      reference.fire('blur'); assert(ui.get('image-preview').hidden, 'blur did not dismiss preview');
    }
  }, { images: true });
  await check('a path at the end of a streamed user message links on END', async ui => {
    await ui.upload(imagePath);
    ui.event({ type: 'delta', id: 'last-path', role: 'user', text: imagePath });
    const body = ui.get('messages').children.at(-1).children[0];
    assert(body.textContent === imagePath, 'pending final path was not visible');
    ui.event({ type: 'end', id: 'last-path' });
    assert(body.children.some(child => child.dataset?.imagePath === imagePath), 'final path was not linked');
  }, { images: true });
  await check('stream boundaries cannot turn an embedded filename substring into a reference', async ui => {
    await ui.upload(imagePath);
    for (const text of ['prefix', imagePath, ' ', imagePath]) ui.event({ type: 'delta', id: 'path-boundary', role: 'user', text });
    ui.event({ type: 'end', id: 'path-boundary' });
    const body = ui.get('messages').children.at(-1).children[0];
    assert(body.textContent === 'prefix' + imagePath + ' ' + imagePath, 'boundary tracking changed text');
    assert(body.children.filter(child => child.dataset?.imagePath).length === 1, 'chunk boundary created a false path reference');
  }, { images: true });
  await check('unknown paths never fetch backend files or become image references', async ui => {
    ui.input('/private/unknown.png'); ui.form(); await ui.idle();
    ui.event({ type: 'delta', id: 'unknown-path', role: 'user', text: '/private/unknown.png' });
    ui.event({ type: 'end', id: 'unknown-path' });
    const body = ui.get('messages').children.at(-1).children[0];
    assert(!body.children.some(child => child.dataset?.imagePath) && !ui.get('image-preview-content').src, 'unknown server file was treated as a local upload');
  });
  await check('preview placement stays within viewport and Escape dismisses it', async ui => {
    await ui.upload(imagePath);
    ui.get('image-preview').rect = { width: 200, height: 160 };
    ui.previews.show(imagePath, { left: 1000, top: 740, bottom: 760 });
    assert(ui.get('image-preview').style.left === '816px' && ui.get('image-preview').style.top === '570px', 'preview overflowed viewport');
    const event = ui.document.fire('keydown', { key: 'Escape' });
    assert(event.defaultPrevented && event.stopped && ui.get('image-preview').hidden, 'Escape did not consume preview dismissal');
  }, { images: true });
  await check('replay reset hides the layer but preserves local previews for replayed paths', async ui => {
    const { url } = await ui.upload(imagePath);
    ui.previews.show(imagePath, { left: 10, top: 100, bottom: 120 });
    ui.event({ type: 'reset' });
    assert(ui.get('image-preview').hidden && !ui.revoked.includes(url), 'replay lost local image ownership');
    ui.event({ type: 'delta', id: 'replayed-path', role: 'user', text: imagePath }); ui.event({ type: 'end', id: 'replayed-path' });
    const reference = ui.get('messages').children[0].children[0].children.find(child => child.dataset?.imagePath);
    reference.fire('pointerenter');
    assert(ui.get('image-preview-content').src === url, 'replayed path cannot show retained local preview');
  }, { images: true });
  await check('local preview cache revokes evicted URLs and cleans up on page departure', async ui => {
    for (let i = 0; i < 64; ++i) ui.previews.remember('/cache/' + i, 'blob:cache-' + i, 'image');
    ui.previews.show('/cache/0', { left: 10, top: 100, bottom: 120 }); ui.previews.hide();
    ui.previews.remember('/cache/64', 'blob:cache-64', 'image');
    assert(ui.revoked.length === 1 && ui.revoked[0] === 'blob:cache-1', 'cache did not retain recently previewed images');
    ui.window.fire('pagehide', { persisted: true });
    assert(ui.revoked.length === 1, 'back-forward cache revoked still-live files');
    ui.window.fire('pagehide', { persisted: false });
    assert(ui.revoked.length === 65 && new Set(ui.revoked).size === 65, 'leaving page leaked or double-revoked local files');
  });
  await check('failed uploads revoke their temporary URL without retaining a preview', async ui => {
    ui.get('image-input').files = [{ name: 'test.jpg', size: 3, slice: () => ({ arrayBuffer: async () => new Uint8Array([255,216,255]).buffer }) }];
    ui.get('image-input').fire('change'); await ui.idle();
    const begin = ui.socket.sent.find(frame => frame.get('action') === 'image_begin');
    ui.event({ type: 'image_error', id: begin.get('id'), text: 'Rejected' }); await ui.idle();
    assert(ui.revoked.includes(ui.blobs[0]) && !ui.prompt.value && ui.get('image-preview').hidden, 'failed upload retained image ownership');
  }, { images: true });
  for (const width of [1, 2, 7, 8192]) {
    const path = '/backend/my € image.png', source = 'prefix' + path + ' then ' + path + ' suffix ' + '/unrelated.png '.repeat(1000);
    let pending = '', output = '', references = 0, previous = '';
    for (let at = 0; at < source.length; at += width) {
      const result = imageReferences(pending + source.slice(at, at + width), [path], false, previous);
      output += result.parts.map(part => part.text).join('');
      references += result.parts.filter(part => part.path).length; pending = result.tail; previous = result.previous;
      assert(pending.length <= path.length, 'stream parser accumulated more than a candidate prefix');
      assert(output + pending === source.slice(0, Math.min(source.length, at + width)), 'incremental path parser lost visible text');
    }
    const result = imageReferences(pending, [path], true, previous);
    assert(output + result.parts.map(part => part.text).join('') === source && references === 1, 'stream parser changed text or matched unrelated paths');
    checks.push('bounded image path parsing with ' + width + '-character chunks');
  }
  await check('clicking a hover thumbnail opens an inset viewer and close releases only its own URL', async ui => {
    const { url } = await ui.upload(imagePath, 'picture.png', 'image/png');
    ui.previews.show(imagePath, { left: 10, top: 100, bottom: 120 });
    ui.get('image-preview').click();
    const pinned = ui.get('image-viewer-content').src;
    assert(ui.get('image-viewer').open && ui.get('image-preview').hidden, 'thumbnail did not become a modal viewer');
    assert(ui.get('image-viewer-title').textContent === 'picture.png' && pinned !== url, 'viewer did not pin the uploaded image');
    assert(ui.prompt.value === imagePath, 'opening the image changed the draft');
    ui.get('image-viewer-close').click();
    assert(!ui.get('image-viewer').open && ui.revoked.includes(pinned) && !ui.revoked.includes(url), 'close leaked its image or discarded the cached preview');
  }, { images: true });
  await check('the hover thumbnail stays available while crossing the gap to click it', async ui => {
    await ui.upload(imagePath);
    ui.previews.show(imagePath, { left: 10, top: 100, bottom: 120 });
    ui.prompt.fire('pointerleave'); ui.get('image-preview').fire('pointerenter'); ui.timeout(150);
    assert(!ui.get('image-preview').hidden, 'hover layer vanished while entering it');
    ui.get('image-preview').click(); assert(ui.get('image-viewer').open, 'retained thumbnail did not open');
  }, { images: true });
  await check('posted path keyboard activation opens the viewer and Escape restores focus', async ui => {
    await ui.upload(imagePath);
    ui.event({ type: 'delta', id: 'open-posted-image', role: 'user', text: imagePath });
    ui.event({ type: 'end', id: 'open-posted-image' });
    const reference = ui.get('messages').children.at(-1).children[0].children.find(child => child.dataset?.imagePath);
    reference.focus(); reference.fire('keydown', { key: 'Enter' });
    assert(ui.get('image-viewer').open && ui.document.activeElement === ui.get('image-viewer-close'), 'viewer did not take keyboard focus');
    const cancel = ui.get('image-viewer').fire('cancel');
    assert(cancel.defaultPrevented && !ui.get('image-viewer').open && ui.document.activeElement === reference, 'Escape did not restore focus');
  }, { images: true });
  await check('an uploading thumbnail can be inspected and copied after the upload is cancelled', async ui => {
    const file = { name: 'uploading.png', type: 'image/png', size: 8,
      slice: () => ({ arrayBuffer: async () => new Uint8Array([137,80,78,71,13,10,26,10]).buffer }) };
    ui.get('image-input').files = [file]; ui.get('image-input').fire('change'); await ui.idle();
    const image = ui.get('uploads').children[0].children[0], original = image.src;
    image.click(); const pinned = ui.get('image-viewer-content').src;
    const begin = ui.socket.sent.find(frame => frame.get('action') === 'image_begin');
    ui.event({ type: 'image_error', id: begin.get('id'), text: 'Cancelled' }); await ui.idle();
    assert(ui.revoked.includes(original) && !ui.revoked.includes(pinned) && ui.get('image-viewer').open, 'upload cancellation revoked the inspected image');
    assert(await ui.previews.viewer.copy() && ui.copiedImages[0] === file, 'cancelled upload could not be copied from its retained file');
    ui.get('image-viewer-close').click(); assert(ui.revoked.includes(pinned), 'viewer did not release its upload pin');
  }, { images: true });
  await check('PNG copy writes image pixels immediately in the click gesture without re-encoding', async ui => {
    const { file } = await ui.upload(imagePath, 'image.png', 'image/png');
    ui.previews.open(imagePath);
    const copied = ui.previews.viewer.copy();
    assert(ui.clipboardCalls.length === 1 && ui.clipboardCalls[0][0].data['image/png']?.then, 'clipboard write waited for preparation or used text instead of image data');
    assert(await copied && ui.copiedImages[0] === file && !ui.canvases.length, 'PNG copy re-encoded or replaced the original pixels');
    assert(ui.get('image-viewer-copy').dataset.copied === '1' && !ui.get('image-viewer-copy').disabled, 'copy feedback did not recover the control');
  }, { images: true });
  await check('JPEG copy converts to PNG and frees its temporary bitmap and URL', async ui => {
    const { url } = await ui.upload(imagePath);
    ui.previews.open(imagePath); const pinned = ui.get('image-viewer-content').src;
    assert(await ui.previews.viewer.copy(), 'JPEG image was not copied');
    assert(ui.copiedImages[0].type === 'image/png' && ui.canvases[0].drawn === ui.decoded[0], 'JPEG copy did not rasterize the original image');
    assert(ui.canvases[0].width === 0 && ui.canvases[0].height === 0, 'JPEG conversion retained its pixel allocation');
    assert(ui.revoked.includes(ui.decoded[0].src) && !ui.revoked.includes(url) && !ui.revoked.includes(pinned), 'JPEG conversion leaked a URL or revoked a live image');
  }, { images: true });
  await check('signature-validated PNG with missing file MIME retains the correct clipboard type', async ui => {
    const { file } = await ui.upload(imagePath, 'image.png', 'image/png');
    file.type = '';
    ui.previews.open(imagePath); assert(await ui.previews.viewer.copy(), 'PNG with missing metadata was not copied');
    assert(ui.copiedImages[0].type === 'image/png' && ui.copiedImages[0].parts[0] === file && !ui.canvases.length, 'PNG MIME repair materialized or converted the original file');
  }, { images: true });
  await check('rendered conversation images open and copy through the same viewer', async ui => {
    const image = ui.document.createElement('img');
    image.src = 'https://images.example/picture.jpg'; image.alt = 'Rendered picture';
    image.closest = selector => selector === '.message .body img' ? image : null;
    const click = ui.document.fire('click', { target: image });
    assert(click.defaultPrevented && ui.get('image-viewer').open && ui.get('image-viewer-content').src === image.src, 'rendered message image did not open');
    assert(await ui.previews.viewer.copy() && ui.copiedImages[0].type === 'image/png', 'rendered image could not be copied');
    assert(ui.decoded[0].crossOrigin === 'anonymous' && ui.decoded[0].src === image.src, 'image copy bypassed ordinary browser image permissions');
  });
  await check('missing image clipboard support gives a visible error while keeping the viewer usable', async ui => {
    await ui.upload(imagePath); ui.previews.open(imagePath);
    assert(!await ui.previews.viewer.copy() && ui.get('toast').textContent.includes('Image copying is unavailable'), 'missing clipboard support was silent');
    assert(ui.get('image-viewer').open && !ui.get('image-viewer-copy').disabled && !ui.copiedImages.length, 'unsupported clipboard damaged viewer state');
  }, { images: true, noImageClipboard: true });
  for (const failure of ['clipboardFailure', 'decodeFailure', 'noCanvas', 'canvasFailure', 'canvasNull']) {
    await check('failed image copy recovers controls and resources: ' + failure, async ui => {
      const { url } = await ui.upload(imagePath); ui.previews.open(imagePath);
      const pinned = ui.get('image-viewer-content').src;
      assert(!await ui.previews.viewer.copy(), 'image copy unexpectedly succeeded');
      await ui.idle();
      assert(ui.get('image-viewer').open && !ui.get('image-viewer-copy').disabled && !ui.copiedImages.length, 'copy failure left unusable controls or wrong clipboard data');
      assert(ui.get('toast').textContent.includes('Could not copy'), 'copy failure was not reported');
      assert(!ui.revoked.includes(url) && !ui.revoked.includes(pinned), 'copy failure revoked the live image');
      for (const decoded of ui.decoded) assert(ui.revoked.includes(decoded.src), 'copy failure leaked its temporary URL');
    }, { images: true, [failure]: true });
  }
  await check('switching viewer images during a pending copy cannot corrupt the new viewer', async ui => {
    await ui.upload(imagePath); ui.previews.open(imagePath);
    const copied = ui.previews.viewer.copy();
    assert(ui.clipboardCalls.length === 1 && ui.get('image-viewer-copy').disabled, 'copy was not dispatched before decoding');
    const temporary = ui.decoded[0].src;
    ui.previews.viewer.open({ url: 'https://images.example/new.png', name: 'New image' });
    assert(!ui.revoked.includes(temporary), 'switching images cancelled the original copy source');
    ui.finishDecode(); assert(await copied, 'pending copy did not complete');
    assert(ui.get('image-viewer-title').textContent === 'New image' && ui.get('image-viewer-copy').dataset.copied === '0' && !ui.get('image-viewer-copy').disabled, 'old copy completion changed the new viewer');
    assert(ui.revoked.includes(temporary), 'completed copy retained its conversion URL');
  }, { images: true, holdDecode: true });
  await check('an inspected image survives preview-cache eviction until its viewer closes', async ui => {
    const { url } = await ui.upload(imagePath, 'image.png', 'image/png'); ui.previews.open(imagePath);
    const pinned = ui.get('image-viewer-content').src;
    for (let i = 0; i < 64; ++i) ui.previews.remember('/evict/' + i, 'blob:evict-' + i, 'image');
    assert(ui.revoked.includes(url) && !ui.revoked.includes(pinned), 'cache eviction revoked the viewer pin');
    assert(await ui.previews.viewer.copy(), 'evicted image lost its retained pixel source');
    ui.previews.viewer.close(); assert(ui.revoked.includes(pinned), 'closing an evicted image leaked its viewer URL');
  }, { images: true });
  await check('reset and image-load failures close the modal and release its retained URL', async ui => {
    await ui.upload(imagePath); ui.previews.open(imagePath);
    const first = ui.get('image-viewer-content').src;
    ui.event({ type: 'reset' });
    assert(!ui.get('image-viewer').open && ui.revoked.includes(first), 'host reset retained the viewer');
    ui.previews.open(imagePath); const second = ui.get('image-viewer-content').src;
    ui.get('image-viewer-content').fire('error');
    assert(!ui.get('image-viewer').open && ui.revoked.includes(second) && ui.get('toast').textContent.includes('could not be loaded'), 'image failure retained a broken modal');
  }, { images: true });
  await check('only clicking the actual backdrop closes the viewer', async ui => {
    await ui.upload(imagePath); ui.previews.open(imagePath);
    ui.get('image-viewer').fire('click', { target: ui.get('image-viewer-content') });
    assert(ui.get('image-viewer').open, 'image inspection closed the viewer');
    ui.get('image-viewer').fire('click', { target: ui.get('image-viewer') });
    assert(!ui.get('image-viewer').open, 'backdrop click retained the viewer');
  }, { images: true });
  return checks;
}
