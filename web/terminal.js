import { Terminal } from './xterm.js';
import { FitAddon } from './xterm-addon-fit.js';

// The transport carries bounded PTY bytes, independently of conversation replay.
// Hosts can mount the same assets and implement this endpoint in their own loop.
export function mountTerminal({ document, window, Terminal, FitAddon }) {
  const get = id => document.getElementById(id), container = get('terminal');
  const embedded = window.parent && window.parent !== window;
  if (embedded) document.documentElement.dataset.embedded = '1';
  const theme = () => {
    const styles = window.getComputedStyle(document.documentElement);
    const color = (name, fallback) => styles.getPropertyValue(name).trim() || fallback;
    return { background: color('--bg', '#0e0e0e'), foreground: color('--text', '#eeeeee'), cursor: color('--accent', '#ffffff') };
  };
  const terminal = new Terminal({ fontFamily: '"JetBrains Mono", monospace', fontSize: 14,
    cursorBlink: true, scrollback: 5000, allowProposedApi: false, disableStdin: true,
    theme: theme() });
  const fit = new FitAddon(); terminal.loadAddon(fit); terminal.open(container);
  const ring = new Uint8Array(65536), encoder = new TextEncoder();
  let head = 0, size = 0, credit = false, ready = false, closed = false;
  let writing = 0, outputBytes = 0, exit = null, frame = null, failure = '';
  let notified = false;
  function notifyClosed() {
    if (embedded && !notified) { notified = true; window.parent.postMessage({ type: 'softline-terminal-closed' }, window.location.origin); }
  }
  const url = new window.URL('./terminal/events', window.location.href);
  url.protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  const socket = new window.WebSocket(url); socket.binaryType = 'arraybuffer';
  const status = text => { get('terminal-status').textContent = get('terminal-status').title = text; };
  const send = (action, fields = {}) => {
    if (socket.readyState !== window.WebSocket.OPEN) return false;
    try { socket.send(new URLSearchParams({ action, ...fields }).toString()); return true; }
    catch { return false; }
  };
  function stop(message) {
    if (closed) return;
    closed = true; ready = false; size = 0; failure = message;
    terminal.options.disableStdin = true; status(message);
    socket.close();
  }
  function flush() {
    if (!ready || !credit || !size || closed) return;
    const bytes = new Uint8Array(Math.min(size, 4096));
    for (let at = 0; at < bytes.length; ++at) bytes[at] = ring[(head + at) % ring.length];
    try { socket.send(bytes); }
    catch { stop('Connection lost. Terminal input was discarded.'); return; }
    head = (head + bytes.length) % ring.length; size -= bytes.length; credit = false;
  }
  function input(text, binary = false) {
    if (closed || socket.readyState !== window.WebSocket.OPEN) return;
    let length = 0;
    for (const character of text) {
      const point = character.codePointAt(0);
      length += binary ? character.length : point < 128 ? 1 : point < 2048 ? 2 : point < 65536 ? 3 : 4;
      if (length > ring.length - size) { stop('Terminal input exceeded 64 KiB while blocked. The session was closed.'); return; }
    }
    const bytes = binary ? Uint8Array.from(text, character => character.charCodeAt(0)) : encoder.encode(text);
    for (const byte of bytes) ring[(head + size++) % ring.length] = byte;
    flush();
  }
  function resize() {
    frame = null;
    try { fit.fit(); }
    catch { /* Initial font/layout measurement may not be available yet. */ }
    if (embedded) {
      const screen = terminal.element?.querySelector('.xterm-screen');
      const height = screen?.getBoundingClientRect().height;
      const rowHeight = height > 0 && terminal.rows > 0 ? height / terminal.rows : Math.ceil(terminal.options.fontSize * 1.2);
      const padding = window.getComputedStyle(container);
      const footer = document.querySelector('.terminal-footer').getBoundingClientRect().height;
      const chromeHeight = footer + (parseFloat(padding.getPropertyValue('padding-top')) || 0) + (parseFloat(padding.getPropertyValue('padding-bottom')) || 0);
      window.parent.postMessage({ type: 'softline-terminal-size', rowHeight, chromeHeight }, window.location.origin);
    }
    if (ready && !closed && !send('resize', { rows: terminal.rows, columns: terminal.cols })) stop('Connection lost.');
  }
  function scheduleResize() {
    if (frame === null && !closed) frame = window.requestAnimationFrame(resize);
  }
  function finishExit() {
    if (exit !== null && !writing) { status(`Shell exited (${exit}).`); notifyClosed(); }
  }
  function close() { stop('Terminal closed.'); if (embedded) notifyClosed(); else window.close(); }
  terminal.onData(text => input(text)); terminal.onBinary(text => input(text, true));
  const observer = new window.ResizeObserver(scheduleResize); observer.observe(container);
  document.fonts?.ready.then(scheduleResize);
  socket.addEventListener('open', () => {
    resize(); status('Opening server shell…');
    if (!send('open', { rows: terminal.rows, columns: terminal.cols })) stop('Could not open terminal.');
  });
  socket.addEventListener('message', event => {
    if (closed) return;
    try {
      if (typeof event.data !== 'string') {
        const bytes = new Uint8Array(event.data);
        if (!ready || !bytes.length || bytes.length > 4096 || outputBytes + bytes.length > 65536) throw new Error('Invalid terminal output.');
        ++writing; outputBytes += bytes.length;
        terminal.write(bytes, () => {
          --writing; outputBytes -= bytes.length;
          if (!closed && exit === null && !send('ack', { bytes: bytes.length })) stop('Connection lost.');
          finishExit();
        });
        return;
      }
      const message = JSON.parse(event.data);
      if (message.type === 'ready' && !ready) {
        ready = true; terminal.options.disableStdin = false; status('Connected'); resize();
        if (!embedded || document.hasFocus()) terminal.focus();
      }
      else if (message.type === 'input_credit' && ready && !credit) { credit = true; flush(); }
      else if (message.type === 'exit' && /^-?\d{1,3}$/u.test(message.status)) {
        exit = message.status; ready = false; terminal.options.disableStdin = true; finishExit();
      } else if (message.type === 'error') stop(message.text || 'The server terminal failed.');
      else throw new Error('Invalid terminal control message.');
    } catch (error) { stop(error.message); }
  });
  socket.addEventListener('close', () => {
    closed = true; ready = false; size = 0; terminal.options.disableStdin = true;
    observer.disconnect();
    if (frame !== null) window.cancelAnimationFrame(frame);
    if (exit !== null) finishExit(); else status(failure || 'Disconnected. The terminal session was closed.');
  });
  socket.addEventListener('error', () => stop('Could not connect to the server terminal.'));
  get('terminal-close').addEventListener('click', close);
  document.addEventListener('keydown', event => {
    if (!embedded || !event.ctrlKey || event.shiftKey || event.altKey || event.metaKey || !(event.key === '`' || event.code === 'Backquote')) return;
    event.preventDefault(); event.stopImmediatePropagation(); if (!event.repeat) close();
  }, true);
  window.addEventListener('message', event => {
    if (!embedded || event.source !== window.parent || event.origin !== window.location.origin) return;
    if (event.data?.type === 'softline-terminal-stop') stop('Terminal closed.');
    else if (event.data?.type === 'softline-terminal-theme' && typeof event.data.theme === 'string' && /^[a-z]+$/u.test(event.data.theme)) {
      document.documentElement.dataset.theme = event.data.theme;
      terminal.options.theme = theme(); scheduleResize();
    }
  });
  window.addEventListener('pagehide', () => stop('Terminal closed.'));
  return { terminal, socket };
}
if (typeof window !== 'undefined' && window.document) {
  try {
    const theme = window.localStorage.getItem('softline.theme');
    if (theme && ['default', 'light', 'gruvbox', 'dracula', 'solarized', 'monokai', 'synthwave', 'plain', 'accent', 'monochrome', 'monogreen', 'outrun', 'riced'].includes(theme)) document.documentElement.dataset.theme = theme;
  } catch {}
  mountTerminal({ document, window, Terminal, FitAddon });
}
