import * as smd from './smd.js';

const NS = 'http://www.w3.org/2000/svg';
const CHARTS = new Set(['mdf-bar-chart', 'mdf-horizontal-bar-chart', 'mdf-vertical-bar-chart', 'mdf-tile-chart']);
const el = (name, attrs = {}, text) => {
  const node = document.createElementNS(NS, name);
  for (const [key, value] of Object.entries(attrs)) node.setAttribute(key, String(value));
  if (text !== undefined) node.textContent = String(text);
  return node;
};
function safeURL(value, image) {
  if (/[\u0000-\u0020\u007f]/u.test(value)) return null;
  try {
    const url = new URL(value, document.baseURI);
    return ['http:', 'https:'].includes(url.protocol) || (!image && url.protocol === 'mailto:') ? value : null;
  } catch { return null; }
}
function csvRow(line) {
  const fields = []; let field = '', quoted = false, closed = false;
  for (let i = 0; i < line.length; ++i) {
    const c = line[i];
    if (quoted) {
      if (c === '"' && line[i + 1] === '"') { field += '"'; ++i; }
      else if (c === '"') { quoted = false; closed = true; }
      else field += c;
    } else if (c === ',' && !quoted) { fields.push(field.trim()); field = ''; closed = false; }
    else if (c === '"' && !field.trim() && !closed) { quoted = true; field = ''; }
    else if (!closed || /\s/u.test(c)) field += c;
    else return null;
  }
  if (quoted) return null;
  fields.push(field.trim());
  return fields.length === 2 ? fields : null;
}
class Chart {
  constructor(code, info) {
    this.code = code; this.pre = code.closest('pre'); this.rows = [];
    this.partial = ''; this.quote = false; this.failed = false; this.bytes = 0;
    const [kind, ...options] = info.split(',').map(x => x.trim()); this.kind = kind;
    this.options = new Map(options.map(option => {
      const [key, value = 'on'] = option.split('=');
      return [key.replaceAll('_', '-'), value];
    }));
    this.figure = document.createElement('figure'); this.figure.className = 'mdf-chart';
    this.figure.setAttribute('aria-label', kind === 'mdf-tile-chart' ? 'Pie chart' : 'Bar chart');
    this.pre.before(this.figure); this.pre.hidden = true;
    this.draw();
  }
  fail() { this.failed = true; cancelAnimationFrame(this.frame); this.figure.remove(); this.pre.hidden = false; }
  write(text) {
    if (this.failed) return;
    this.bytes += text.length;
    if (this.bytes > 65536) { this.fail(); return; }
    for (const c of text) {
      if (c === '"') this.quote = !this.quote;
      if (c === '\n' && !this.quote) { this.row(this.partial); this.partial = ''; }
      else this.partial += c;
    }
    if (!this.frame && !this.failed) this.frame = requestAnimationFrame(() => { this.frame = 0; this.draw(); });
  }
  row(line) {
    if (this.failed || !line.trim()) return;
    const fields = csvRow(line);
    if (!fields) { this.fail(); return; }
    const [label, raw] = fields;
    const numeric = /^[+-]?((\d+(\.\d*)?)|(\.\d+))([eE][+-]?\d+)?$/u.test(raw);
    const value = numeric ? Number(raw) : NaN;
    if (!Number.isFinite(value) || value < 0) {
      if (!numeric && !this.header && !this.rows.length) { this.header = true; return; }
      this.fail(); return;
    }
    if (this.rows.length >= 256) { this.fail(); return; }
    this.rows.push({ label, value, index: this.rows.length });
    if (!Number.isFinite(this.rows.reduce((sum, r) => sum + r.value, 0))) this.fail();
  }
  end() { cancelAnimationFrame(this.frame); this.frame = 0; if (this.partial.trim()) this.row(this.partial); this.partial = ''; this.draw(); }
  draw() {
    if (this.failed) return;
    const rows = [...this.rows], sort = this.options.get('sort');
    if (['on', 'desc', 'descending'].includes(sort)) rows.sort((a, b) => b.value - a.value || a.index - b.index);
    if (['asc', 'ascending'].includes(sort)) rows.sort((a, b) => a.value - b.value || a.index - b.index);
    const total = rows.reduce((sum, r) => sum + r.value, 0);
    const colored = !['off', 'no', 'false', '0'].includes(this.options.get('colored-bars'));
    const percent = !['on', 'yes', 'true', '1'].includes(this.options.get('disable-percentage') ?? this.options.get('disable-percent'));
    const color = i => colored ? `var(--c${i % 6})` : 'var(--accent)';
    const svg = el('svg', { viewBox: this.kind === 'mdf-tile-chart' ? '0 0 480 260' : '0 0 640 320', role: 'img' });
    svg.append(el('title', {}, rows.map(r => `${r.label}: ${r.value}`).join(', ') || 'Waiting for chart data'));
    if (this.kind === 'mdf-tile-chart') {
      if (!total) svg.append(el('circle', { cx: 240, cy: 130, r: 110, class: 'chart-empty' }));
      let angle = -Math.PI / 2;
      rows.forEach((row, i) => {
        if (!row.value || !total) return;
        const end = angle + 2 * Math.PI * row.value / total;
        const attrs = { fill: color(i), stroke: 'var(--code)', 'stroke-width': 1 };
        const node = row.value === total ? el('circle', { ...attrs, cx: 240, cy: 130, r: 110 }) : el('path', {
          ...attrs, d: `M240 130 L${240 + 110 * Math.cos(angle)} ${130 + 110 * Math.sin(angle)} A110 110 0 ${end - angle > Math.PI ? 1 : 0} 1 ${240 + 110 * Math.cos(end)} ${130 + 110 * Math.sin(end)} Z`
        });
        node.append(el('title', {}, `${row.label}: ${row.value} (${(row.value / total * 100).toFixed(1)}%)`));
        svg.append(node); angle = end;
      });
    } else if (this.kind === 'mdf-vertical-bar-chart') {
      const max = Math.max(0, ...rows.map(r => r.value)) || 1, slot = 580 / Math.max(1, rows.length);
      svg.append(el('line', { x1: 30, x2: 610, y1: 270, y2: 270, class: 'chart-axis' }));
      rows.forEach((row, i) => {
        const height = 230 * row.value / max;
        const bar = el('rect', { x: 30 + i * slot + slot * .15, y: 270 - height, width: slot * .7, height, rx: 2, fill: color(i) });
        bar.append(el('title', {}, `${row.label}: ${row.value}`)); svg.append(bar);
        if (slot >= 35) svg.append(el('text', { x: 30 + (i + .5) * slot, y: 290, 'text-anchor': 'middle' }, row.label.length > 12 ? row.label.slice(0, 11) + '…' : row.label));
        if (slot >= 30) svg.append(el('text', { x: 30 + (i + .5) * slot, y: 262 - height, 'text-anchor': 'middle' }, `${row.value}${percent ? ` · ${(total ? row.value / total * 100 : 0).toFixed(1)}%` : ''}`));
      });
    } else {
      const height = Math.max(80, rows.length * 34 + 20), max = Math.max(0, ...rows.map(r => r.value)) || 1;
      svg.setAttribute('viewBox', `0 0 640 ${height}`); svg.style.maxHeight = `${Math.min(600, height)}px`;
      rows.forEach((row, i) => {
        const y = i * 34 + 12;
        svg.append(el('text', { x: 0, y: y + 15 }, row.label.length > 20 ? row.label.slice(0, 19) + '…' : row.label));
        const bar = el('rect', { x: 160, y, width: 340 * row.value / max, height: 23, rx: 3, fill: color(i) });
        bar.append(el('title', {}, row.label)); svg.append(bar);
        svg.append(el('text', { x: 515, y: y + 15 }, `${row.value}${percent ? ` · ${(total ? row.value / total * 100 : 0).toFixed(1)}%` : ''}`));
      });
    }
    const legend = document.createElement('figcaption'); legend.className = 'chart-legend';
    rows.forEach((row, i) => {
      const item = document.createElement('span'), key = document.createElement('i');
      key.className = 'chart-key'; key.style.background = color(i); item.append(key);
      item.append(document.createTextNode(`${row.label} ${row.value}${this.kind === 'mdf-tile-chart' ? ` · ${(total ? row.value / total * 100 : 0).toFixed(1)}%` : ''}`)); legend.append(item);
    });
    this.figure.replaceChildren(svg, legend);
  }
}

/* Only parser state and bounded chart/construct state are retained. Completed
 * text lives in the DOM, never in a growing source string for reparsing. */
export class MarkdownStream {
  constructor(root) {
    this.renderer = smd.default_renderer(root); this.charts = new Map();
    const original = { ...this.renderer };
    this.renderer.add_token = (data, type) => {
      if (data.index >= 22) throw new Error('Markdown nesting limit exceeded');
      original.add_token(data, type);
    };
    this.renderer.set_attr = (data, type, value) => {
      if (type === smd.HREF || type === smd.SRC) {
        const safe = safeURL(value, type === smd.SRC);
        if (safe === null) return;
        original.set_attr(data, type, safe);
        if (type === smd.HREF) data.nodes[data.index].setAttribute('rel', 'noopener noreferrer');
        if (type === smd.SRC) data.nodes[data.index].setAttribute('loading', 'lazy');
      } else if (type === smd.LANG && CHARTS.has(value.split(',')[0].trim())) {
        this.charts.set(data.index, new Chart(data.nodes[data.index], value));
      } else original.set_attr(data, type, value);
    };
    this.renderer.add_text = (data, text) => {
      original.add_text(data, text); this.charts.get(data.index)?.write(text);
    };
    this.renderer.end_token = data => {
      this.charts.get(data.index)?.end(); this.charts.delete(data.index);
      original.end_token(data);
    };
    this.parser = smd.parser(this.renderer);
    this.root = root; this.failed = false;
  }
  write(text) {
    if (this.failed) { this.root.append(document.createTextNode(text)); return; }
    // Feed Unicode scalars so any bounded-lookahead fallback can retain the
    // exact unconsumed suffix, including a character split between byte deltas.
    for (const char of text) {
      if (this.failed) { this.root.append(document.createTextNode(char)); continue; }
      if (this.parser.pending.length >= 8192 || this.renderer.data.index >= 20) {
        this.failed = true;
        this.root.append(document.createTextNode(this.parser.pending + char));
        this.parser.pending = '';
        continue;
      }
      try { smd.parser_write(this.parser, char); }
      catch {
        this.failed = true;
        this.root.append(document.createTextNode(this.parser.pending + char));
        this.parser.pending = '';
      }
    }
  }
  end() {
    if (!this.failed) smd.parser_end(this.parser);
    for (const chart of this.charts.values()) chart.end();
    this.charts.clear();
  }
}
