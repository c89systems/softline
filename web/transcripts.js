import { Terminal } from './xterm.js';

const maxChunk = 16 * 1024;
const maxPending = 256 * 1024;
const encoder = new TextEncoder();

function size(event) {
  const rows = Number(event.rows), cols = Number(event.columns);
  return Number.isInteger(rows) && rows >= 1 && rows <= 200 &&
    Number.isInteger(cols) && cols >= 1 && cols <= 320 ? { rows, cols } : null;
}

export class Transcripts {
  constructor(document, window) {
    this.document = document;
    this.window = window;
    this.toggle = document.getElementById('transcript-toggle');
    this.panel = document.getElementById('transcripts');
    this.select = document.getElementById('transcript-select');
    this.screen = document.getElementById('transcript-screen');
    this.views = new Map();
    this.active = '';
    this.toggle.addEventListener('click', () => this.cycle());
    this.select.addEventListener('change', () => this.show(this.select.value));
    window.addEventListener('keydown', event => {
      if (event.defaultPrevented || event.key !== 'F8' || event.repeat ||
          event.isComposing || event.altKey || event.ctrlKey || event.metaKey ||
          event.shiftKey || document.querySelector('dialog[open]') || !this.views.size) return;
      event.preventDefault();
      this.cycle();
    }, { capture: true });
  }

  colors() {
    const styles = this.window.getComputedStyle(this.document.documentElement);
    return {
      background: styles.getPropertyValue('--bg').trim(),
      foreground: styles.getPropertyValue('--text').trim()
    };
  }

  theme() {
    for (const view of this.views.values()) {
      if (view.terminal) view.terminal.options.theme = this.colors();
    }
  }

  show(id) {
    const next = this.views.get(id);
    const previous = this.views.get(this.active);
    if (previous && previous !== next) previous.stage.hidden = true;
    this.active = next ? id : '';
    this.select.value = this.active;
    this.panel.hidden = !next;
    this.toggle.setAttribute('aria-expanded', String(!!next));
    if (!next) return;
    next.stage.hidden = false;
    if (next.terminal && !next.terminal.element) next.terminal.open(next.stage);
    else if (next.terminal) next.terminal.refresh(0, next.terminal.rows - 1);
  }

  cycle() {
    const ids = [...this.views.keys()];
    this.show(ids[ids.indexOf(this.active) + 1] || '');
  }

  createTerminal(view) {
    view.pending = 0;
    view.stage.replaceChildren();
    view.terminal = new Terminal({
      rows: view.rows, cols: view.cols, disableStdin: true, scrollback: 2000,
      fontFamily: '"JetBrains Mono", monospace', fontSize: 13, theme: this.colors()
    });
    // Screen-local operations are handled by xterm. Ignore links and clipboard
    // commands from the producer. There is no onData/onBinary transport.
    for (const code of [8, 52]) view.terminal.parser.registerOscHandler(code, () => true);
  }

  remove(id) {
    const view = this.views.get(id);
    if (!view) return;
    if (this.active === id) this.show('');
    view.terminal?.dispose();
    view.stage.remove();
    view.option.remove();
    this.views.delete(id);
    this.toggle.hidden = !this.views.size;
  }

  clear() {
    for (const id of this.views.keys()) this.remove(id);
  }

  enqueue(view, bytes, parsed) {
    const terminal = view.terminal;
    if (!terminal) return;
    const weight = Math.max(64, bytes.length);
    if (view.pending + weight > maxPending) {
      terminal.dispose();
      view.terminal = null;
      view.stage.textContent = 'Transcript fell behind. Waiting for the host to reset it.';
      return;
    }
    view.pending += weight;
    terminal.write(bytes, () => {
      if (terminal !== view.terminal) return;
      view.pending -= weight;
      parsed?.(terminal);
    });
  }

  write(view, event) {
    if (!view.terminal) return;
    const vt = event.encoding === 'base64';
    let bytes;
    if (vt) {
      if (typeof event.data !== 'string' || event.data.length > 4 * Math.ceil(maxChunk / 3)) return;
      try { bytes = Uint8Array.from(atob(event.data), char => char.charCodeAt(0)); } catch { return; }
      if (bytes.length > maxChunk) return;
    } else {
      if (typeof event.text !== 'string' || event.text.length > maxChunk) return;
      const source = encoder.encode(event.text);
      if (source.length > maxChunk) return;
      bytes = encoder.encode(event.text.replace(/[\x00-\x08\x0b-\x1f\x7f-\x9f]/gu, '').replace(/\n/gu, '\r\n'));
    }
    if (bytes.length) this.enqueue(view, bytes);
  }

  receive(event) {
    const { id } = event;
    if (typeof id !== 'string' || !id || id.length > 80) return;
    const view = this.views.get(id);
    if (event.type === 'transcript_open') {
      const geometry = size(event);
      if (view || this.views.size >= 8 || !geometry || typeof event.title !== 'string') return;
      const stage = this.document.createElement('div');
      stage.className = 'transcript-stage';
      stage.hidden = true;
      const option = this.document.createElement('option');
      option.value = id;
      option.textContent = event.title.slice(0, 80);
      const entry = { ...geometry, stage, option };
      this.createTerminal(entry);
      this.screen.append(stage);
      this.select.append(option);
      this.views.set(id, entry);
      this.toggle.hidden = false;
    } else if (view) {
      switch (event.type) {
        case 'transcript_data': this.write(view, event); break;
        case 'transcript_resize': {
          const geometry = size(event);
          if (!geometry) break;
          Object.assign(view, geometry);
          // xterm parses writes asynchronously. Order geometry with the
          // bytes on both sides of the resize without collecting the stream.
          this.enqueue(view, '', terminal => terminal.resize(geometry.cols, geometry.rows));
          break;
        }
        case 'transcript_reset':
          view.terminal?.dispose();
          this.createTerminal(view);
          if (this.active === id) this.show(id);
          break;
        case 'transcript_remove': this.remove(id); break;
      }
    }
  }
}
