const space = c => !!c && /[ \t\n\r\v\f]/u.test(c);
const difference = (before, after) => {
  let at = 0, tail = 0;
  while (at < before.length && at < after.length && before[at] === after[at]) ++at;
  while (tail < before.length - at && tail < after.length - at && before[before.length - 1 - tail] === after[after.length - 1 - tail]) ++tail;
  return { at, removed: before.length - at - tail, inserted: after.length - at - tail };
};
function clusterEnd(text, at) {
  if (typeof Intl !== 'undefined' && Intl.Segmenter) {
    for (const part of new Intl.Segmenter(undefined, { granularity: 'grapheme' }).segment(text)) {
      const end = part.index + part.segment.length;
      if (part.index < at && at < end) return end;
    }
  } else {
    if (at && /[\ud800-\udbff]/u.test(text[at - 1]) && /[\udc00-\udfff]/u.test(text[at])) ++at;
    while (at < text.length && /\p{Mark}/u.test(String.fromCodePoint(text.codePointAt(at)))) at += String.fromCodePoint(text.codePointAt(at)).length;
  }
  return at;
}

// The same completion rules as the native editor: a provider snapshot per
// activation, separate continuation levels, and a stable original Tab cycle.
// Only edit()/finish() mutate the real draft; projection() never does.
export class Completion {
  constructor(current, edit, resolve, redraw, canActivate) {
    this.current = current; this.edit = edit; this.resolve = resolve; this.redraw = redraw;
    this.canActivate = canActivate;
    this.providers = new Map(); this.session = null; this.revision = 0;
  }
  provider(rule) { this.providers.set(rule.trigger, rule); }
  reset() { ++this.revision; this.session = null; this.redraw(); }
  active() { return !!this.session && !this.session.literal; }
  async level(session, prefix) {
    if (!session.levels.has(prefix)) {
      const entry = { items: [], pending: true };
      session.levels.set(prefix, entry);
      entry.promise = this.resolve(prefix, session).then(items => {
        entry.items = [...new Set(items)]; entry.pending = false; this.redraw(); return entry.items;
      }, () => {
        entry.pending = false; entry.failed = true;
        if (prefix === session.rule.trigger) session.failed = true;
        this.redraw(); return [];
      });
    }
    return session.levels.get(prefix).promise;
  }
  activate(text, cursor, literalInput = false) {
    if (this.session || literalInput || !cursor || !this.canActivate()) return;
    let start = cursor;
    while (start && !space(text[start - 1])) --start;
    const rule = this.providers.get(text[start]);
    if (!rule || (start && rule.scope === 'start')) return;
    let end = cursor;
    while (end < text.length && !space(text[end])) ++end;
    const session = this.session = { rule, start, end, levels: new Map(), cycle: null, committed: false, failed: false };
    void this.level(session, rule.trigger);
  }
  finish() {
    ++this.revision;
    const s = this.session;
    this.session = null;
    const { text, cursor } = this.current();
    if (s && !s.literal && !s.failed && s.rule.strip && text[s.start] === s.rule.trigger) {
      this.edit(text.slice(0, s.start) + text.slice(s.start + 1), cursor > s.start ? cursor - 1 : cursor,
        { at: s.start, removed: 1, syntax: true });
    }
    this.redraw();
  }
  cursor(activate = true) {
    ++this.revision;
    const s = this.session, { text, cursor, end = cursor } = this.current();
    if (s && (cursor <= s.start || cursor > s.end || end > s.end || (!s.literal && text[s.start] !== s.rule.trigger))) this.finish();
    if (this.session) { this.session.cycle = null; this.session.committed = false; }
    if (activate) this.activate(this.current().text, this.current().cursor);
    this.redraw();
  }
  input(before, after, cursor, literalInput = false, edit) {
    const s = this.session;
    if (s) {
      const d = edit || difference(before, after);
      s.cycle = null; s.committed = false;
      if (d.at <= s.start || d.at > s.end || d.removed > s.end - d.at) this.session = null;
      else s.end += d.inserted - d.removed;
    }
    this.cursor(false);
    if (this.session && !this.session.literal && !this.list().length) {
      let start = this.current().cursor;
      const text = this.current().text;
      while (start && !space(text[start - 1])) --start;
      if (start > this.session.start && this.providers.get(text[start])?.scope === 'word') this.finish();
    }
    this.activate(this.current().text, this.current().cursor, literalInput);
    this.redraw();
  }
  list(s = this.session) {
    if (!s || s.literal || s.failed) return [];
    const { text, cursor } = this.current(), prefix = text.slice(s.start, cursor);
    let best = -1, items = [];
    for (const [key, entry] of s.levels) {
      if (!entry.failed && entry.items.length && key.length > best && prefix.startsWith(key) && entry.items.some(item => item.startsWith(prefix))) {
        best = key.length; items = entry.items;
      }
    }
    return items.filter(item => item.startsWith(prefix));
  }
  offer() {
    const s = this.session, { text, cursor, end = cursor } = this.current();
    if (!s || s.literal || s.failed || s.committed || cursor !== end || cursor <= s.start || cursor > s.end) return null;
    const items = this.list(), prefix = text.slice(s.start, cursor);
    if (!items.length || (items.length === 1 && items[0].length === prefix.length && !(cursor < text.length && !space(text[cursor - 1]) && !space(text[cursor])))) return null;
    return { items, prefix, first: items[0] };
  }
  projection() {
    const { text, cursor } = this.current(), offer = this.offer();
    if (!offer || this.session.cycle) return { before: text, ghost: '', after: '' };
    const suffix = offer.first.slice(offer.prefix.length);
    let overlap = 0;
    while (overlap < suffix.length && suffix[overlap] === text[cursor + overlap]) ++overlap;
    if (overlap && /[\ud800-\udbff]/u.test(suffix[overlap - 1]) && /[\udc00-\udfff]/u.test(suffix[overlap])) --overlap;
    return { before: text.slice(0, cursor + overlap), ghost: suffix.slice(overlap), after: text.slice(cursor + overlap) };
  }
  async ready() {
    const s = this.session, revision = this.revision;
    if (s && !s.literal) await this.level(s, s.rule.trigger);
    return this.session === s && revision === this.revision ? s : null;
  }
  ancestor(s) {
    const { text, cursor } = this.current(), prefix = text.slice(s.start, cursor);
    let best = '';
    for (const entry of s.levels.values()) for (const item of entry.items) if (item.length > best.length && prefix.startsWith(item)) best = item;
    return best;
  }
  async expand(s) {
    if (!s.rule.expand) return;
    let previous = '';
    const revision = this.revision;
    for (;;) {
      const prefix = this.ancestor(s);
      if (!prefix || prefix.length <= previous.length) return;
      previous = prefix;
      await this.level(s, prefix);
      if (this.session !== s || revision !== this.revision || this.offer()) return;
    }
  }
  candidate(original, cursor, start, item) {
    let end = cursor;
    while (end < original.length && !space(original[end])) ++end;
    const extension = item.slice(cursor - start);
    if (original.slice(cursor).startsWith(extension) && end < cursor + extension.length) end = cursor + extension.length;
    const text = original.slice(0, start) + item + original.slice(end);
    const at = clusterEnd(text, start + item.length);
    // Tab cycles rebuild from the original token. Include any original suffix
    // restored by a shorter candidate, or consumed by a longer one.
    const cycle = this.session.cycle;
    const previousEnd = cycle.originalEnd ?? end;
    const currentEnd = (cycle.insertedEnd ?? end) + Math.max(0, end - previousEnd);
    this.edit(text, at, { at: start, removed: currentEnd - start });
    cycle.originalEnd = end;
    cycle.insertedEnd = start + item.length;
    this.session.end = at;
  }
  async tab() {
    const revision = this.revision;
    const s = await this.ready();
    if (!s || s.literal || s.failed) return false;
    if (!s.cycle) {
      if (!this.offer()) await this.expand(s);
      if (this.session !== s || revision !== this.revision || !this.offer()) return false;
      const { text, cursor } = this.current();
      s.cycle = { original: text, cursor, items: this.list(s), selected: 0 };
    } else s.cycle.selected = (s.cycle.selected + 1) % s.cycle.items.length;
    this.candidate(s.cycle.original, s.cycle.cursor, s.start, s.cycle.items[s.cycle.selected]);
    s.committed = false; this.redraw(); return true;
  }
  async commitPrefix() {
    const revision = this.revision;
    const s = await this.ready();
    if (!s || s.literal) return;
    s.cycle = null; s.committed = false;
    if (s.rule.expand) {
      const { text, cursor } = this.current();
      const items = await this.level(s, text.slice(s.start, cursor));
      if (this.session !== s || revision !== this.revision) return;
      if (items.length && this.offer()) { this.redraw(); return; }
    }
    s.committed = true; this.redraw();
  }
  async right() {
    const s = await this.ready();
    if (!s || s.literal) return false;
    if (s.cycle) { await this.commitPrefix(); return true; }
    const { cursor } = this.current();
    if (cursor !== s.end) return false;
    if (this.offer()) { await this.tab(); await this.commitPrefix(); return true; }
    const prefix = this.current().text.slice(s.start, cursor);
    if (!s.committed && prefix && s.rule.expand && this.ancestor(s) === prefix) { await this.commitPrefix(); return true; }
    return false;
  }
  async separator(character) {
    await this.ready();
    const s = this.session, { text, cursor } = this.current();
    if (s && (s.literal || !this.list(s).some(item => item.startsWith(text.slice(s.start, cursor) + character)))) this.finish();
    if (this.session) { this.session.cycle = null; this.session.committed = false; }
  }
  escape() {
    ++this.revision;
    if (this.session) { this.session.cycle = null; this.session.committed = false; }
    this.redraw();
  }
  adopt(start, end, literal = false) {
    this.session = null;
    if (literal) this.session = { start, end, literal: true };
    else this.activate(this.current().text, this.current().cursor);
    this.redraw();
  }
}
