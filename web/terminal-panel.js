// A layout panel around the locally served terminal view. The iframe owns its
// PTY transport; removing it closes that transport without touching chat replay.
export class TerminalPanel {
  constructor({ document, window, closed }) {
    this.document = document; this.window = window; this.closed = closed;
    this.origin = window.origin || window.location.origin;
    this.panel = document.getElementById('terminal-panel');
    this.rowHeight = 18; this.chromeHeight = 40;
    window.addEventListener('message', event => {
      if (!this.frame || event.source !== this.frame.contentWindow || event.origin !== this.origin) return;
      const data = event.data;
      if (data?.type === 'softline-terminal-closed') this.close();
      else if (data?.type === 'softline-terminal-size' && Number.isFinite(data.rowHeight) && data.rowHeight >= 8 && data.rowHeight <= 100 && Number.isFinite(data.chromeHeight) && data.chromeHeight >= 0 && data.chromeHeight <= 300) {
        this.rowHeight = data.rowHeight; this.chromeHeight = data.chromeHeight; this.resize();
      }
    });
    window.addEventListener('resize', () => this.resize());
    window.visualViewport?.addEventListener('resize', () => this.resize());
    this.observer = new window.ResizeObserver(() => this.resize());
    this.observer.observe(document.getElementById('composer-area'));
    this.observer.observe(document.getElementById('workspace'));
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
    this.frame = frame; this.panel.replaceChildren(frame); this.panel.hidden = false;
    this.document.getElementById('chat').classList.remove('welcome-state');
    this.resize();
    frame.focus({ preventScroll: true });
  }
  resize() {
    if (!this.frame) return;
    const workspace = this.document.getElementById('workspace').getBoundingClientRect();
    const composer = this.document.getElementById('composer-area').getBoundingClientRect();
    const viewport = this.window.visualViewport?.height || this.window.innerHeight;
    const available = Math.max(0, Math.min(workspace.height, viewport - workspace.top));
    const maximum = Math.max(0, available - composer.height - Math.min(80, available / 5));
    // The panel's top border is outside the iframe's measured terminal chrome.
    const height = Math.min(Math.max(Math.ceil(25 * this.rowHeight + this.chromeHeight + 1), viewport / 3), maximum);
    const value = `${Math.floor(height)}px`;
    if (this.panel.style.height !== value) this.panel.style.height = value;
  }
  theme() {
    this.frame?.contentWindow.postMessage({ type: 'softline-terminal-theme', theme: this.document.documentElement.dataset.theme }, this.origin);
  }
  close() {
    if (!this.frame) return;
    const frame = this.frame; this.frame = null;
    frame.contentWindow.postMessage({ type: 'softline-terminal-stop' }, this.origin);
    this.panel.hidden = true; this.panel.replaceChildren();
    this.closed();
  }
}
