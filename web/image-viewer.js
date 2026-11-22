export class ImageViewer {
  constructor(document, view, notice) {
    this.document = document; this.view = view; this.notice = notice;
    this.dialog = document.getElementById('image-viewer');
    this.image = document.getElementById('image-viewer-content');
    this.title = document.getElementById('image-viewer-title');
    this.copyButton = document.getElementById('image-viewer-copy');
    this.closeButton = document.getElementById('image-viewer-close');
    this.copyRun = 0;
    this.closeButton.addEventListener('click', () => this.close());
    this.copyButton.addEventListener('click', () => this.copy());
    this.dialog.addEventListener('cancel', event => { event.preventDefault(); this.close(); });
    this.dialog.addEventListener('close', () => { if (!this.dialog.open) this.release(); });
    this.dialog.addEventListener('click', event => { if (event.target === this.dialog) this.close(); });
    this.image.addEventListener('error', () => {
      if (this.source) { this.notice('The image could not be loaded.'); this.close(); }
    });
  }
  open(source) {
    if (!source?.url) return;
    this.release(); this.source = source;
    // Give the viewer its own URL so upload cancellation or cache eviction
    // cannot revoke an image that is still being inspected.
    if (source.file) this.ownedURL = this.view.URL.createObjectURL(source.file);
    this.image.src = this.ownedURL || source.url;
    this.image.alt = this.title.textContent = source.name || 'Image';
    if (!this.dialog.open) this.dialog.showModal();
    this.closeButton.focus();
  }
  release() {
    ++this.copyRun; this.source = null;
    this.image.removeAttribute('src');
    if (this.ownedURL) this.view.URL.revokeObjectURL(this.ownedURL);
    this.ownedURL = null;
    this.copyButton.disabled = false; this.copyButton.title = 'Copy image'; this.copyButton.dataset.copied = '0';
  }
  close() {
    this.release();
    if (this.dialog.open) this.dialog.close();
  }
  async png(source) {
    if (source.file && source.type === 'image/png') {
      return source.file.type === 'image/png' ? source.file : new this.view.Blob([source.file], { type: 'image/png' });
    }
    let canvas, ownedURL;
    try {
      const image = new this.view.Image();
      image.crossOrigin = 'anonymous';
      if (source.file) ownedURL = this.view.URL.createObjectURL(source.file);
      image.src = ownedURL || source.url;
      await image.decode();
      if (!image.naturalWidth || !image.naturalHeight) throw new Error('The image has no pixels.');
      canvas = this.document.createElement('canvas');
      canvas.width = image.naturalWidth; canvas.height = image.naturalHeight;
      const context = canvas.getContext('2d');
      if (!context) throw new Error('Image conversion is unavailable.');
      context.drawImage(image, 0, 0);
      return await new Promise((resolve, reject) => canvas.toBlob(blob => {
        if (blob) resolve(blob); else reject(new Error('Image conversion failed.'));
      }, 'image/png'));
    } finally {
      if (canvas) canvas.width = canvas.height = 0;
      if (ownedURL) this.view.URL.revokeObjectURL(ownedURL);
    }
  }
  async copy() {
    if (!this.source || this.copyButton.disabled) return false;
    const clipboard = this.view.navigator?.clipboard;
    if (!clipboard?.write || !this.view.ClipboardItem) {
      this.notice('Image copying is unavailable. Use HTTPS or a loopback address and a browser with image clipboard support.');
      return false;
    }
    const run = ++this.copyRun, source = this.source;
    this.copyButton.disabled = true;
    try {
      // Invoke write during the click gesture, supplying asynchronous pixels
      // as a promise. Awaiting JPEG conversion first loses activation in WebKit.
      const pixels = this.png(source);
      pixels.catch(() => {});
      await clipboard.write([new this.view.ClipboardItem({ 'image/png': pixels })]);
      if (run === this.copyRun) {
        this.copyButton.title = 'Image copied'; this.copyButton.dataset.copied = '1';
        this.notice('Image copied.');
      }
      return true;
    } catch {
      if (run === this.copyRun) this.notice('Could not copy the image. Clipboard access was denied or the image is not readable.');
      return false;
    } finally {
      if (run === this.copyRun) this.copyButton.disabled = false;
    }
  }
}
