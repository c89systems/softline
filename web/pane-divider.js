export class PaneDivider {
  constructor(handle, workspace, resize) {
    this.handle = handle;
    this.workspace = workspace;
    this.resize = resize;
    handle.addEventListener('pointerdown', event => {
      if (event.button !== 0 || !event.isPrimary || workspace.dataset.paneResizing ||
          this.minimum === this.maximum) return;
      handle.setPointerCapture(event.pointerId);
      handle.focus({ preventScroll: true });
      this.drag = { id: event.pointerId, start: this.position(event), size: this.size };
      workspace.dataset.paneResizing = this.axis;
      event.preventDefault();
    });
    handle.addEventListener('pointermove', event => {
      if (this.drag?.id !== event.pointerId) return;
      this.setSize(this.drag.size + this.direction * (this.position(event) - this.drag.start));
    });
    for (const type of ['pointerup', 'pointercancel', 'lostpointercapture']) {
      handle.addEventListener(type, event => {
        if (this.drag?.id === event.pointerId) this.cancel();
      });
    }
    handle.addEventListener('keydown', event => {
      if (event.altKey || event.ctrlKey || event.metaKey || event.isComposing) return;
      const lower = this.axis === 'x' ? 'ArrowLeft' : 'ArrowUp';
      const higher = this.axis === 'x' ? 'ArrowRight' : 'ArrowDown';
      const step = event.shiftKey ? 40 : 10;
      let size;
      if (event.key === lower) size = this.size - this.direction * step;
      else if (event.key === higher) size = this.size + this.direction * step;
      else if (event.key === 'Home') size = this.minimum;
      else if (event.key === 'End') size = this.maximum;
      else return;
      event.preventDefault();
      this.setSize(size);
    });
  }

  position(event) {
    return this.axis === 'x' ? event.clientX : event.clientY;
  }

  setSize(size) {
    this.resize(Math.round(Math.min(this.maximum, Math.max(this.minimum, size))), this.axis);
  }

  update(axis, direction, size, minimum, maximum) {
    if (axis !== this.axis) this.cancel();
    this.axis = axis;
    this.direction = direction;
    this.size = size;
    this.minimum = minimum;
    this.maximum = maximum;
    this.handle.setAttribute('aria-orientation', axis === 'x' ? 'vertical' : 'horizontal');
    this.handle.setAttribute('aria-valuemin', String(minimum));
    this.handle.setAttribute('aria-valuemax', String(maximum));
    this.handle.setAttribute('aria-valuenow', String(size));
    this.handle.setAttribute('aria-valuetext', `${size} pixels`);
  }

  cancel() {
    if (!this.drag) return;
    const { id } = this.drag;
    this.drag = null;
    delete this.workspace.dataset.paneResizing;
    if (this.handle.hasPointerCapture(id)) this.handle.releasePointerCapture(id);
  }
}
