import { ImageViewer } from './image-viewer.js';

const boundary = c => !c || /[\s"'()[\]{},;:]/u.test(c);

// Only an unfinished known-path prefix is carried between streamed chunks.
// It remains visible as plain text until it can become a reference.
export function imageReferences(text, paths, final = true, previous = '') {
  let end = text.length;
  if (!final) for (let at = text.lastIndexOf('/'); at >= 0; at = text.lastIndexOf('/', at - 1)) {
    if (boundary(at ? text[at - 1] : previous) && paths.some(path => path.startsWith(text.slice(at)))) end = at;
    if (!at) break;
  }
  const parts = [];
  let at = 0;
  while (at < end) {
    let first = end, selected = '';
    for (const path of paths) {
      let found = text.indexOf(path, at);
      while (found >= 0 && found + path.length <= end) {
        if (boundary(found ? text[found - 1] : previous) && boundary(text[found + path.length])) {
          if (found < first || (found === first && path.length > selected.length)) { first = found; selected = path; }
          break;
        }
        found = text.indexOf(path, found + 1);
      }
    }
    if (first > at) parts.push({ text: text.slice(at, first) });
    if (!selected) break;
    parts.push({ text: selected, path: selected }); at = first + selected.length;
  }
  return { parts, tail: text.slice(end), previous: end ? text[end - 1] : previous };
}

export class ImagePreviews {
  constructor(document, view, { limit = 64, notice = () => {} } = {}) {
    this.document = document; this.view = view; this.limit = limit; this.images = new Map();
    this.prompt = document.getElementById('prompt');
    this.panel = document.getElementById('image-preview');
    this.image = document.getElementById('image-preview-content');
    this.viewer = new ImageViewer(document, view, notice);
    this.draftSpans = [];
    this.prompt.addEventListener('pointermove', event => this.hoverDraft(event));
    this.prompt.addEventListener('pointerleave', event => this.later(event));
    for (const name of ['input', 'blur', 'scroll']) this.prompt.addEventListener(name, () => this.hide());
    this.panel.addEventListener('pointerenter', () => view.clearTimeout(this.hideTimer));
    this.panel.addEventListener('pointerleave', event => this.later(event));
    this.panel.addEventListener('click', () => { if (this.current) this.open(this.current.path); });
    this.panel.addEventListener('keydown', event => {
      if ((event.key === 'Enter' || event.key === ' ') && this.current) { event.preventDefault(); this.open(this.current.path); }
    });
    document.addEventListener('scroll', () => this.hide(), true);
    document.addEventListener('pointerdown', event => { if (!event.target.closest('.image-reference,#image-preview')) this.hide(); });
    document.addEventListener('click', event => {
      const image = event.target.closest('.message .body img');
      if (image && !event.defaultPrevented) {
        event.preventDefault(); this.hide();
        this.viewer.open({ url: image.currentSrc || image.src, name: image.alt || 'Image' });
      }
    });
    document.addEventListener('keydown', event => {
      if (event.key === 'Escape' && this.current) { event.preventDefault(); event.stopImmediatePropagation(); this.hide(); }
    }, true);
    view.addEventListener('resize', () => this.hide());
    view.addEventListener('pagehide', event => { if (event.persisted) this.hide(); else this.clear(); });
    this.image.addEventListener('load', () => this.place());
    this.image.addEventListener('error', () => this.hide());
  }
  remember(path, url, name, file, type) {
    const previous = this.images.get(path);
    if (previous) { this.hide(); this.view.URL.revokeObjectURL(previous.url); this.images.delete(path); }
    this.images.set(path, { url, name: name || 'Uploaded image', file, type });
    while (this.images.size > this.limit) {
      const oldest = this.images.keys().next().value;
      if (this.current?.path === oldest) this.hide();
      this.view.URL.revokeObjectURL(this.images.get(oldest).url); this.images.delete(oldest);
    }
  }
  clear() {
    this.viewer.close();
    this.hide();
    for (const image of this.images.values()) this.view.URL.revokeObjectURL(image.url);
    this.images.clear(); this.draftSpans = [];
  }
  hide() {
    this.view.clearTimeout(this.hideTimer);
    this.current = null; this.panel.hidden = true;
    this.image.removeAttribute('src'); this.prompt.style.cursor = '';
  }
  show(path, anchor, draft = false) {
    if (this.viewer.dialog.open) return;
    this.view.clearTimeout(this.hideTimer);
    const image = this.images.get(path);
    if (!image) { this.hide(); return; }
    this.images.delete(path); this.images.set(path, image);
    if (this.current?.path !== path) { this.image.alt = image.name; this.image.src = image.url; }
    this.current = { path, anchor, draft }; this.panel.hidden = false; this.place();
  }
  later(event) {
    if (event?.relatedTarget && this.panel.contains(event.relatedTarget)) return;
    this.view.clearTimeout(this.hideTimer);
    this.hideTimer = this.view.setTimeout(() => this.hide(), 150);
  }
  open(path) {
    const source = this.images.get(path);
    if (source) { this.hide(); this.viewer.open(source); }
  }
  inspect(image, source) {
    image.tabIndex = 0; image.setAttribute('role', 'button'); image.title = 'Open image';
    const open = event => { event.preventDefault(); this.hide(); this.viewer.open(source); };
    image.addEventListener('click', open);
    image.addEventListener('keydown', event => { if (event.key === 'Enter' || event.key === ' ') open(event); });
  }
  place() {
    if (!this.current) return;
    const box = this.panel.getBoundingClientRect(), anchor = this.current.anchor;
    const width = this.view.innerWidth, height = this.view.innerHeight;
    const left = Math.max(8, Math.min(anchor.left, width - box.width - 8));
    const above = anchor.top - box.height - 10;
    const top = Math.max(8, Math.min(above >= 8 ? above : anchor.bottom + 10, height - box.height - 8));
    this.panel.style.left = left + 'px'; this.panel.style.top = top + 'px';
  }
  reference(path, interactive) {
    const span = this.document.createElement('span');
    span.textContent = path; span.dataset.imagePath = path;
    if (interactive) {
      span.className = 'image-reference'; span.tabIndex = 0;
      span.setAttribute('role', 'button'); span.setAttribute('aria-label', 'Preview image: ' + path);
      const show = () => this.show(path, span.getBoundingClientRect());
      span.addEventListener('pointerenter', show); span.addEventListener('pointerleave', event => this.later(event));
      span.addEventListener('focus', show); span.addEventListener('blur', () => this.hide());
      span.addEventListener('click', () => this.open(path));
      span.addEventListener('keydown', event => {
        if (event.key === 'Enter' || event.key === ' ') { event.preventDefault(); this.open(path); }
      });
    } else this.draftSpans.push(span);
    return span;
  }
  render(root, text, final = true, interactive = true, previous = '') {
    const result = imageReferences(text, [...this.images.keys()], final, previous);
    let tail = null;
    for (const part of result.parts) root.append(part.path ? this.reference(part.path, interactive) : this.document.createTextNode(part.text));
    if (result.tail) {
      tail = this.document.createTextNode(result.tail); root.append(tail);
    }
    return { tail, previous: result.previous };
  }
  append(root, text, message, final = false) {
    // Retire only the bounded, still-visible prefix node, never the transcript.
    if (message.imageTail) { text = message.imageTail.data + text; message.imageTail.remove(); }
    const result = this.render(root, text, final, true, message.imageBefore);
    message.imageTail = result.tail; message.imageBefore = result.previous;
  }
  draft() {
    this.draftSpans = [];
    for (const id of ['projection-before', 'projection-after']) {
      const root = this.document.getElementById(id), text = root.textContent;
      root.replaceChildren(); this.render(root, text, true, false);
    }
    if (this.current?.draft) this.hide();
  }
  hoverDraft(event) {
    if (event.buttons) { this.hide(); return; }
    const bounds = this.prompt.getBoundingClientRect(), x = event.clientX, y = event.clientY;
    if (x < bounds.left || x >= bounds.right || y < bounds.top || y >= bounds.bottom) { this.hide(); return; }
    for (const span of this.draftSpans) for (const rect of span.getClientRects()) {
      if (x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom) {
        this.show(span.dataset.imagePath, rect, true); this.prompt.style.cursor = 'zoom-in'; return;
      }
    }
    this.later();
  }
}
