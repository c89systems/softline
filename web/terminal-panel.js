import { PaneDivider } from './pane-divider.js';

// A layout panel around the locally served terminal view. The iframe owns its
// PTY transport; removing it closes that transport without touching chat replay.
export class TerminalPanel {
  constructor({ document, window, closed }) {
    this.document = document; this.window = window; this.closed = closed;
    this.origin = window.origin || window.location.origin;
    this.panel = document.getElementById('terminal-panel');
    this.conversation = document.getElementById('conversation-pane');
    this.handle = document.getElementById('terminal-divider');
    this.divider = new PaneDivider(this.handle, document.getElementById('workspace'), this.panel, height => {
      this.height = height;
      this.resize();
    });
    this.rowHeight = 18; this.chromeHeight = 40;
    window.addEventListener('message', event => {
      if (!this.frame || event.source !== this.frame.contentWindow || event.origin !== this.origin) return;
      const data = event.data;
      if (data?.type === 'softline-terminal-closed') this.close();
      else if (data?.type === 'softline-terminal-size' && Number.isFinite(data.rowHeight) && data.rowHeight >= 8 && data.rowHeight <= 100 && Number.isFinite(data.chromeHeight) && data.chromeHeight >= 0 && data.chromeHeight <= 300) {
        this.frameMounted = true;
        this.rowHeight = data.rowHeight; this.chromeHeight = data.chromeHeight; this.resize();
      }
    });
    window.addEventListener('resize', () => this.resize());
    window.visualViewport?.addEventListener('resize', () => this.resize());
    this.observer = new window.ResizeObserver(() => this.resize());
    this.observer.observe(document.getElementById('composer-area'));
    this.observer.observe(this.conversation);
  }
  open(path) {
    if (this.frame) { this.frame.contentWindow.focus(); return; }
    const url = new this.window.URL(path, this.document.baseURI || this.window.location.href);
    if (url.origin !== this.origin || !['http:', 'https:'].includes(url.protocol)) throw new Error('The terminal must use this chat origin.');
    this.rowHeight = 18; this.chromeHeight = 40;
    const frame = this.document.createElement('iframe');
    frame.title = 'Server terminal'; frame.className = 'terminal-frame';
    frame.addEventListener('load', () => this.theme());
    frame.src = url.href;
    const chat = this.document.getElementById('chat');
    chat.dispatchEvent(new Event('softline:composer-dock'));
    this.frameMounted = false;
    this.frame = frame; this.panel.replaceChildren(frame);
    this.divider.show('y');
    chat.classList.remove('welcome-state');
    this.resize();
    frame.focus({ preventScroll: true });
  }
  resize() {
    if (!this.frame) return;
    const conversation = this.conversation.getBoundingClientRect();
    const composer = this.document.getElementById('composer-area').getBoundingClientRect();
    const viewport = this.window.visualViewport?.height || this.window.innerHeight;
    const available = Math.max(0, Math.min(conversation.height, viewport - conversation.top));
    const maximum = Math.max(0, Math.floor(available - composer.height -
      Math.min(80, available / 5) - this.handle.getBoundingClientRect().height));
    const minimum = Math.min(maximum, Math.ceil(3 * this.rowHeight + this.chromeHeight));
    const preferred = this.height ?? Math.max(25 * this.rowHeight + this.chromeHeight, viewport / 3);
    const height = Math.floor(Math.min(maximum, Math.max(minimum, preferred)));
    const value = `${height}px`;
    if (this.panel.style.getPropertyValue('--pane-size') !== value)
      this.panel.style.setProperty('--pane-size', value);
    this.divider.update('y', -1, height, minimum, maximum);
  }
  theme() {
    this.frame?.contentWindow.postMessage({ type: 'softline-terminal-theme', theme: this.document.documentElement.dataset.theme }, this.origin);
  }
  close() {
    if (!this.frame) return;
    const frame = this.frame; this.frame = null;
    frame.contentWindow.postMessage({ type: 'softline-terminal-stop' }, this.origin);
    // A loading frame may have missed the stop message and never opened a PTY.
    if (!this.frameMounted) frame.remove();
    this.divider.hide(() => frame.remove());
    this.closed();
  }
}
