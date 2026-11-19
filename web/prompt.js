import { Completion } from './completion.js';

const space = c => /[ \t\n\r\v\f]/u.test(c);
function boundary(text, at, forward) {
  if (typeof Intl !== 'undefined' && Intl.Segmenter) {
    const parts = [...new Intl.Segmenter(undefined, { granularity: 'grapheme' }).segment(text)].map(part => part.index);
    return forward ? parts.find(index => index > at) ?? text.length : parts.reverse().find(index => index < at) ?? 0;
  }
  const points = [...text.slice(0, at)];
  return forward ? Math.min(text.length, at + (text.codePointAt(at) > 65535 ? 2 : 1)) : at - (points.pop()?.length || 0);
}
export class Prompt {
  constructor(options) {
    this.options = options;
    const get = this.get = id => options.document.getElementById(id);
    this.input = get('prompt'); this.menu = get('completions');
    this.requests = new Map(); this.serial = 0; this.menuOpen = false;
    this.menuItems = []; this.menuActive = 0; this.jobs = []; this.busy = false;
    this.before = this.input.value; this.literalInput = false;
    this.model = new Completion(() => this.current(), (text, cursor, edit) => this.apply(text, cursor, edit),
      (prefix, session) => this.resolve(prefix, session), () => this.render());
    this.input.addEventListener('beforeinput', event => this.beforeInput(event));
    this.input.addEventListener('input', event => this.changed(event));
    this.input.addEventListener('keydown', event => this.key(event));
    this.input.addEventListener('click', () => { this.model.cursor(); this.hideMenu(); });
    const selection = () => {
      if (this.selection?.[0] !== this.input.selectionStart || this.selection?.[1] !== this.input.selectionEnd) {
        this.model.cursor(); this.hideMenu();
      }
    };
    this.input.addEventListener('select', selection);
    options.document.addEventListener('selectionchange', () => { if (options.document.activeElement === this.input) selection(); });
    this.input.addEventListener('keyup', event => {
      if (['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown', 'Home', 'End'].includes(event.key) && !this.menuOpen) selection();
    });
    this.input.addEventListener('scroll', () => this.syncScroll());
    new ResizeObserver(() => this.render()).observe(get('plus').parentElement || this.input);
    get('plus').addEventListener('click', () => this.openMenu());
    options.document.addEventListener('click', event => { if (!event.target.closest('#completions,#plus')) this.hideMenu(); });
    options.document.addEventListener('keydown', event => {
      if (event.defaultPrevented || event.isComposing || event.key !== 'Escape' || event.altKey || event.ctrlKey || event.metaKey || event.shiftKey) return;
      const active = options.document.activeElement;
      if (active === this.input || event.target.closest?.('input,textarea,select,[contenteditable]:not([contenteditable="false"])')) return;
      if (this.focus()) event.preventDefault();
    });
    this.focus(true);
  }
  focus(automatic = false) {
    const document = this.options.document, active = document.activeElement;
    if (document.querySelector('dialog[open]')) return false;
    if (automatic && (document.hidden || active && active !== document.body && active !== document.documentElement)) return false;
    this.hideMenu();
    this.input.focus({ preventScroll: true });
    return true;
  }
  current() { return { text: this.input.value, cursor: this.input.selectionStart, end: this.input.selectionEnd }; }
  apply(text, cursor, edit = {}) {
    if (new TextEncoder().encode(text).length > 128 * 1024) throw new Error('The prompt exceeds 128 KiB.');
    const before = this.input.value, oldEnd = this.input.selectionEnd;
    this.input.value = text;
    this.input.setSelectionRange(cursor, edit.syntax === undefined ? cursor : oldEnd > edit.syntax ? oldEnd - 1 : oldEnd);
    this.options.mutate(before, text, edit.syntax); this.before = text;
    this.render();
  }
  render() {
    this.selection = [this.input.selectionStart, this.input.selectionEnd];
    const projection = this.model.projection(), ghost = this.get('completion-ghost');
    this.get('projection-before').textContent = projection.before;
    ghost.textContent = projection.ghost;
    this.get('projection-after').textContent = projection.after;
    this.get('prompt-projection').hidden = !projection.ghost;
    this.input.dataset.ghost = projection.ghost ? '1' : '0';
    this.options.resize(); this.syncScroll();
  }
  syncScroll() {
    const projection = this.get('prompt-projection');
    projection.scrollTop = this.input.scrollTop; projection.scrollLeft = this.input.scrollLeft;
  }
  finish() { this.model.finish(); this.hideMenu(); }
  reset() { this.model.reset(); this.before = this.input.value; this.render(); }
  connected() {
    const literal = this.model.session?.literal ? this.model.session : null;
    this.model.reset();
    if (literal) this.model.adopt(literal.start, literal.end, true);
    else this.model.activate(this.input.value, this.input.selectionStart, this.literalInput);
  }
  disconnected() {
    for (const request of this.requests.values()) { clearTimeout(request.timer); request.reject(new Error('Disconnected.')); }
    this.requests.clear(); this.hideMenu();
  }
  resolve(prefix, session) {
    if (this.requests.size >= 32) return Promise.reject(new Error('Too many completion requests.'));
    const request = String(++this.serial), current = this.current(), bytes = new TextEncoder();
    return new Promise((resolve, reject) => {
      const entry = { resolve, reject, items: [], prefix };
      entry.timer = setTimeout(() => { this.requests.delete(request); reject(new Error('Completion timed out.')); }, 3000);
      this.requests.set(request, entry);
      if (!this.options.send('complete', { request, text: current.text, cursor: bytes.encode(current.text.slice(0, current.cursor)).length,
        start: bytes.encode(current.text.slice(0, session.start)).length, end: bytes.encode(current.text.slice(0, session.end)).length,
        resolve: prefix, all: '0' })) {
        clearTimeout(entry.timer); this.requests.delete(request); reject(new Error('Disconnected.'));
      }
    });
  }
  receive(event) {
    if (event.type === 'completion_provider') {
      if (typeof event.trigger === 'string' && event.trigger.length === 1 && ['start', 'word'].includes(event.scope)) {
        this.model.provider({ trigger: event.trigger, scope: event.scope, strip: event.strip === '1', expand: event.expand === '1' });
        this.model.activate(this.input.value, this.input.selectionStart, this.literalInput);
      }
      return;
    }
    const request = this.requests.get(event.request);
    if (request) {
      if (event.type === 'complete' && event.candidate !== undefined) {
        if (typeof event.candidate !== 'string' || !event.candidate.startsWith(request.prefix) ||
            (request.prefix.length > 1 && event.candidate.length <= request.prefix.length) ||
            /[\u0000-\u0008\u000b-\u001f\u007f-\u009f]/u.test(event.candidate)) request.failed = true;
        else request.items.push(event.candidate);
      }
      if (event.type === 'complete_end') {
        clearTimeout(request.timer); this.requests.delete(event.request);
        if (request.failed || event.status !== 'ok') request.reject(new Error('Completion provider failed.'));
        else request.resolve(request.items);
      }
      return;
    }
    if (event.request !== this.menuRequest || !this.menuOpen || this.menuText !== this.input.value || this.menuCursor !== this.input.selectionStart) return;
    if (event.type === 'complete' && event.value !== undefined) { this.menuItems.push(event); this.showMenu(); }
    if (event.type === 'complete_end' && event.status !== 'ok') { this.menuItems = []; this.showMenu(); }
  }
  enqueue(action) {
    this.jobs.push(action); this.drain();
  }
  drain() {
    if (this.busy) return;
    while (this.jobs.length) {
      let result;
      try { result = this.jobs.shift()(); }
      catch (error) { this.model.escape(); this.options.toast(error.message); continue; }
      if (result?.then) {
        this.busy = true;
        result.catch(error => { this.model.escape(); this.options.toast(error.message); }).finally(() => { this.busy = false; this.drain(); });
        return;
      }
    }
  }
  beforeInput(event) {
    const literal = ['insertFromPaste', 'insertFromDrop', 'insertFromPasteAsQuotation'].includes(event.inputType);
    const data = event.data ?? event.dataTransfer?.getData('text/plain');
    const replayable = ['insertText', 'insertFromPaste', 'insertFromDrop', 'insertFromPasteAsQuotation', 'insertLineBreak', 'insertParagraph', 'deleteContentBackward', 'deleteContentForward'].includes(event.inputType) &&
      (!event.inputType.startsWith('insert') || ['insertLineBreak', 'insertParagraph'].includes(event.inputType) || typeof data === 'string');
    if (event.cancelable !== false && replayable && (this.busy || (data && space(data) && !literal))) {
      event.preventDefault(); this.enqueue(() => this.editInput(event.inputType, data || '', literal)); return;
    }
    // Native undo, word deletion and composition invalidate pending acceptance.
    // Never cancel an edit we cannot reproduce.
    if (literal) this.finish();
    if (this.busy && !replayable) this.model.reset();
    this.literalInput = literal; this.before = this.input.value;
  }
  changed(event) {
    if (event.isComposing) this.model.reset();
    const before = this.before;
    this.options.mutate(before, this.input.value);
    this.model.input(before, this.input.value, this.input.selectionStart, this.literalInput);
    this.before = this.input.value; this.hideMenu();
  }
  async editInput(type, data, literal = false) {
    if (literal) this.finish();
    if (type === 'insertLineBreak' || type === 'insertParagraph') data = '\n';
    if (type.startsWith('insert')) {
      for (const character of literal ? [data] : [...data]) {
        if (!literal && space(character)) await this.model.separator(character);
        const before = this.input.value;
        this.input.setRangeText(character, this.input.selectionStart, this.input.selectionEnd, 'end');
        this.options.mutate(before, this.input.value);
        this.model.input(before, this.input.value, this.input.selectionStart, literal);
      }
    } else if (type === 'deleteContentBackward' || type === 'deleteContentForward') {
      let start = this.input.selectionStart, end = this.input.selectionEnd;
      if (start === end) {
        if (type === 'deleteContentBackward') start = boundary(this.input.value, start, false);
        else end = boundary(this.input.value, end, true);
      }
      const before = this.input.value;
      this.input.setRangeText('', start, end, 'end'); this.options.mutate(before, this.input.value);
      this.model.input(before, this.input.value, this.input.selectionStart);
    }
    this.literalInput = literal; this.before = this.input.value; this.hideMenu(); this.render();
  }
  move(key) {
    const { text, cursor, end } = this.current();
    let at = cursor;
    if (key === 'ArrowLeft') at = end !== cursor ? cursor : boundary(text, cursor, false);
    else if (key === 'ArrowRight') at = end !== cursor ? end : boundary(text, cursor, true);
    else if (key === 'Home') at = text.lastIndexOf('\n', cursor - 1) + 1;
    else if (key === 'End') { const next = text.indexOf('\n', cursor); at = next < 0 ? text.length : next; }
    if (at !== cursor || this.input.selectionEnd !== cursor) {
      this.input.setSelectionRange(at, at); this.model.cursor();
    }
  }
  key(event) {
    if (event.isComposing) return;
    const control = event.ctrlKey && !event.altKey, key = event.key;
    const shortcut = !event.altKey && !event.metaKey && event.ctrlKey &&
      (event.shiftKey ? { Enter: 'edit', Digit1: 'approvals', Digit2: 'approve', Digit3: 'reject' }[event.code || key] : key === '`' || event.code === 'Backquote' ? 'terminal' : null);
    if (shortcut && this.options.action) {
      event.preventDefault();
      this.hideMenu();
      if (!event.repeat) this.options.action(shortcut);
      return;
    }
    if (control && key.toLowerCase() === 'c' && this.options.cancelImages()) { event.preventDefault(); return; }
    if (this.menuOpen && ['ArrowDown', 'ArrowUp', 'Tab'].includes(key)) {
      event.preventDefault();
      for (let tries = 0; tries < this.menuList.length; ++tries) {
        this.menuActive = (this.menuActive + (key === 'ArrowUp' ? -1 : 1) + this.menuList.length) % this.menuList.length;
        if (!this.menuList[this.menuActive].disabled) break;
      }
      this.showMenu(); return;
    }
    if (key === 'Escape') {
      const menu = this.menuOpen; this.hideMenu(); this.model.escape();
      if (!menu) this.options.cancel(); return;
    }
    if (key === 'Enter' && !event.shiftKey) {
      event.preventDefault();
      if (this.menuOpen && !event.altKey) {
        const item = this.menuList[this.menuActive];
        if (item?.action) this.choose(item);
        else this.enqueue(() => this.choose(item));
      }
      else this.enqueue(() => this.options.submit(event.altKey));
      return;
    }
    if (key === 'Tab' && this.model.active()) { event.preventDefault(); this.enqueue(() => this.model.tab()); return; }
    if (((key === 'ArrowRight' && !event.ctrlKey) || (control && key.toLowerCase() === 'f')) && !event.shiftKey && !event.altKey && !event.metaKey && this.model.active()) {
      event.preventDefault(); this.enqueue(async () => {
        const revision = this.model.revision;
        if (!await this.model.right() && revision === this.model.revision) this.move('ArrowRight');
      }); return;
    }
    if (key === ' ' || (key === 'Enter' && event.shiftKey)) {
      if (this.model.active() || this.busy) { event.preventDefault(); this.enqueue(() => this.editInput('insertText', key === ' ' ? ' ' : '\n')); }
      return;
    }
    const movement = control ? { a: 'Home', b: 'ArrowLeft', e: 'End', f: 'ArrowRight' }[key.toLowerCase()] : this.busy && ['ArrowLeft', 'ArrowRight', 'Home', 'End'].includes(key) ? key : null;
    if (movement && !event.shiftKey && !event.altKey && !event.metaKey) { event.preventDefault(); this.enqueue(() => this.move(movement)); return; }
    if (this.busy && ['Backspace', 'Delete'].includes(key)) {
      event.preventDefault(); this.enqueue(() => this.editInput(key === 'Backspace' ? 'deleteContentBackward' : 'deleteContentForward', ''));
    }
  }
  hideMenu() { this.menuOpen = false; this.menuRequest = null; this.menu.hidden = true; this.get('plus').setAttribute('aria-expanded', 'false'); }
  openMenu() {
    this.finish(); this.menuOpen = true; this.menuItems = []; this.menuActive = 0;
    this.menuText = this.input.value; this.menuCursor = this.input.selectionStart; this.menuRequest = String(++this.serial);
    this.options.send('complete', { request: this.menuRequest, text: this.menuText, cursor: new TextEncoder().encode(this.menuText.slice(0, this.menuCursor)).length, all: '1' });
    this.showMenu();
  }
  showMenu() {
    if (!this.menuOpen) return;
    const list = [...this.menuItems];
    list.unshift(...(this.options.actions?.() || []).map(action => ({ ...action, action: action.id })));
    if (this.options.images()) list.unshift({ image: true, label: 'Add images', description: 'PNG or JPEG · also drop images here' });
    for (const item of this.options.recovery()) list.unshift({ recover: true, source: item, text: item.text, label: 'Recover unsent prompt', description: item.text.slice(0, 60) });
    if (!list[this.menuActive] || list[this.menuActive].disabled) this.menuActive = Math.max(0, list.findIndex(item => !item.disabled));
    this.menuList = list; this.menu.replaceChildren();
    list.forEach((item, index) => {
      const button = this.options.document.createElement('button'), label = this.options.document.createElement('span'),
        symbol = this.options.document.createElement('span'), description = this.options.document.createElement('span');
      button.type = 'button'; button.className = `completion${index === this.menuActive ? ' active' : ''}`;
      button.disabled = !!item.disabled;
      button.setAttribute('aria-disabled', String(!!item.disabled));
      button.setAttribute('role', 'option'); button.setAttribute('aria-selected', String(index === this.menuActive));
      symbol.className = 'completion-icon'; symbol.textContent = item.image ? '▧' : item.label?.startsWith('/') ? '/' : '↳';
      description.className = 'description'; description.textContent = item.description || '';
      label.textContent = item.label || item.value; button.append(symbol, label, description);
      button.addEventListener('click', () => item.action ? this.choose(item) : this.enqueue(() => this.choose(item))); this.menu.append(button);
    });
    this.menu.hidden = !list.length; this.get('plus').setAttribute('aria-expanded', String(!!list.length));
  }
  async choose(item) {
    if (!item || item.disabled) return;
    this.finish();
    if (item.action) { this.options.action(item.action); return; }
    if (item.image) { this.options.imagePicker(); return; }
    if (item.recover) {
      const { text, cursor, end } = this.current();
      const value = (text ? '\n' : '') + item.text;
      this.apply(text.slice(0, cursor) + value + text.slice(end), cursor + value.length);
      this.options.recovered(item.source); this.model.reset(); return;
    }
    const bytes = new TextEncoder().encode(this.input.value), decode = at => new TextDecoder().decode(bytes.slice(0, Number(at))).length;
    const start = decode(item.start || 0), end = decode(item.end || item.start || 0);
    this.apply(this.input.value.slice(0, start) + item.value + this.input.value.slice(end), start + item.value.length);
    this.model.adopt(start, start + item.value.length, item.continue !== '1');
    if (item.continue === '1') await this.model.commitPrefix();
    this.input.focus();
  }
}
