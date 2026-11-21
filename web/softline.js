import { MarkdownStream } from './markdown.js';
import { Prompt } from './prompt.js';
import { ImagePreviews } from './images.js';
import { TerminalPanel } from './terminal-panel.js';
import { Transcripts } from './transcripts.js';

function randomId() {
  return Array.from(crypto.getRandomValues(new Uint32Array(4)), value => value.toString(16).padStart(8, '0')).join('');
}
const $ = id => document.getElementById(id);
const prompt = $('prompt'), scroll = $('scroll'), messages = $('messages');
const previews = new ImagePreviews(document, window, { notice: toast });
const state = { socket: null, online: false, follow: true, messages: new Map(),
  queue: new Map(), seq: '', upload: null,
  view: randomId(), generation: 0, imageLimit: 32 * 1024 * 1024,
  queueDelay: 500, queueTimer: null, pending: new Map(), failed: [], epoch: null,
  images: true, attachments: [], requestSeq: 0, approval: null, edit: null,
  editPending: null, canEdit: false, terminal: '', promptDisabled: false, restorePromptAfterTerminal: false,
  promptExplicitDisabled: false, promptReason: '', terminalPromptLock: false, terminalHandoffs: 0,
  terminalUnlockCommand: '' };
const icon = () => $('copy-icon').content.cloneNode(true);
let toastTimer, simulation = null, simulationInput = null;
function toast(text) { $('toast').textContent = text; $('toast').hidden = false; clearTimeout(toastTimer); toastTimer = setTimeout(() => { $('toast').hidden = true; }, 3500); }
function preference(id, fallback) {
  let value = fallback;
  try { value = localStorage.getItem(`softline.${id}`) || fallback; } catch {}
  if (![...$(id).options].some(option => option.value === value)) value = fallback;
  $(id).value = value; document.documentElement.dataset[id] = value;
  $(id).addEventListener('change', () => {
    document.documentElement.dataset[id] = $(id).value;
    try { localStorage.setItem(`softline.${id}`, $(id).value); } catch {}
    if (id === 'theme') { terminalPanel.theme(); transcripts.theme(); }
  });
}
preference('theme', 'default'); preference('font', 'sans');
function send(action, fields = {}) {
  if (state.online && state.socket?.readyState === WebSocket.OPEN) {
    try { state.socket.send(new URLSearchParams({ action, view: state.view, ...fields }).toString()); return true; } catch {}
  }
  toast('Disconnected. Your draft is kept.'); return false;
}
function resizePrompt() {
  // Simulated input only appends, so it can measure growth at the current height.
  if (!simulation) prompt.style.height = 'auto';
  const projection = $('prompt-projection');
  if (prompt.clientWidth) projection.style.width = `${prompt.clientWidth}px`;
  const height = `${Math.min(Math.max(prompt.scrollHeight, projection.hidden ? 0 : projection.scrollHeight), 200)}px`;
  if (prompt.style.height !== height) prompt.style.height = height;
  if (prompt.clientWidth) projection.style.width = `${prompt.clientWidth}px`;
  $('send').disabled = state.promptDisabled || !state.online || !prompt.value.trim() || !!state.attachments.length;
  previews.draft();
}
function follow() { if (state.follow) scroll.scrollTop = scroll.scrollHeight; }
scroll.addEventListener('scroll', () => {
  state.follow = scroll.scrollHeight - scroll.clientHeight - scroll.scrollTop < 65;
  $('latest').hidden = state.follow;
}, { passive: true });
$('latest').addEventListener('click', () => { state.follow = true; follow(); $('latest').hidden = true; });
let promptWidth = prompt.clientWidth;
const followResize = new ResizeObserver(() => {
  if (prompt.clientWidth !== promptWidth) {
    promptWidth = prompt.clientWidth;
    resizePrompt();
  }
  follow();
});
followResize.observe(messages); followResize.observe(scroll); followResize.observe(prompt);
function started() { $('chat').classList.remove('welcome-state'); }
function message(id, role = 'assistant') {
  if (state.messages.has(id)) return state.messages.get(id);
  if (role !== 'note') started();
  const article = document.createElement('article'), body = document.createElement('div');
  article.className = `message ${['user', 'assistant', 'note'].includes(role) ? role : 'assistant'}`;
  article.dataset.message = id; body.className = 'body'; article.append(body);
  const result = { article, body, role, raw: [], decoder: new TextDecoder(), stream: role === 'user' ? null : new MarkdownStream(body) };
  state.messages.set(id, result); messages.append(article);
  if (role !== 'user') {
    const tools = document.createElement('div'), copy = document.createElement('button');
    tools.className = 'message-tools'; copy.type = 'button'; copy.className = 'copy';
    copy.title = 'Copy response'; copy.setAttribute('aria-label', 'Copy response'); copy.append(icon());
    copy.addEventListener('click', async () => {
      try { await navigator.clipboard.writeText(result.raw.join('')); copy.classList.add('copied'); copy.title = 'Copied'; setTimeout(() => { copy.classList.remove('copied'); copy.title = 'Copy response'; }, 1500); }
      catch { toast('Copy was unavailable. Use HTTPS or a loopback address to copy the original Markdown.'); }
    });
    tools.append(copy); article.append(tools);
  }
  follow(); return result;
}
function queue() {
  clearTimeout(state.queueTimer);
  const restoreFocus = $('queue').contains(document.activeElement);
  const focused = state.approval && state.queue.get(state.approval.id);
  if (!focused || focused.revision !== state.approval.revision || focused.held !== '1') state.approval = null;
  $('queue').replaceChildren();
  const oldest = [...state.queue.values()].find(item => item.held === '1');
  if (state.approval?.id !== oldest?.id) state.approval = null;
  let due = Infinity;
  for (const item of state.queue.values()) {
    if (item.held !== '1' && item.visibleAt > performance.now()) { due = Math.min(due, item.visibleAt); continue; }
    const row = document.createElement('div'), label = document.createElement('b'), text = document.createElement('span');
    row.className = `queue-item${state.approval?.id === item.id ? ' approval-focused' : ''}`; label.className = 'queue-badge';
    row.tabIndex = -1;
    row.dataset.id = item.id;
    row.addEventListener('keydown', event => editor.key(event));
    label.textContent = item.held === '1' ? 'A' : item.mode === 'steer' ? 'S' : item.mode === 'pending' ? 'P' : 'Q';
    text.textContent = item.text; row.append(label, text);
    for (const action of item.held === '1' ? ['approve', 'edit', 'reject'] : ['edit']) {
      if (action === 'edit' && !state.canEdit) continue;
      const button = document.createElement('button'); button.type = 'button'; button.textContent = { approve: 'Approve', edit: 'Edit', reject: 'Reject' }[action];
      button.disabled = !state.online || item.mode === 'pending' || (item.held === '1' && oldest !== item);
      button.addEventListener('click', () => action === 'edit' ? editQueue(item) : decide(action, item)); row.append(button);
    }
    $('queue').append(row);
  }
  $('queue').hidden = !$('queue').childElementCount;
  if (restoreFocus) {
    const row = state.approval && [...$('queue').children].find(item => item.dataset.id === state.approval.id);
    (row || prompt).focus();
  }
  if (Number.isFinite(due)) state.queueTimer = setTimeout(queue, Math.max(0, due - performance.now()));
  editor?.showMenu();
}
function decide(action, item) {
  const oldest = [...state.queue.values()].find(candidate => candidate.held === '1');
  if (!item || item !== oldest) { toast('Focus the oldest approval first.'); return; }
  if (send(action, { id: item.id, revision: item.revision })) { state.approval = null; queue(); prompt.focus(); }
}
function editQueue(item) {
  if (!state.canEdit || !state.online || !item || item.mode === 'pending') { toast('No editable queued message is available.'); return; }
  if (state.edit) { $('queue-editor').showModal(); $('queue-edit-text').focus(); return; }
  state.edit = { id: item.id, revision: item.revision, original: item.text };
  $('queue-edit-save').disabled = $('queue-edit-cancel').disabled = $('queue-edit-text').disabled = false;
  $('queue-edit-text').value = item.text; $('queue-edit-error').textContent = '';
  $('queue-editor').showModal(); $('queue-edit-text').focus();
}
function actionItems() {
  const oldest = [...state.queue.values()].find(item => item.held === '1');
  const focused = state.approval && state.queue.get(state.approval.id);
  return [
    { id: 'focus', label: 'Focus prompt', description: 'Esc' },
    { id: 'terminal', label: terminalPanel.frame ? 'Close terminal' : 'Open terminal', description: state.terminal ? 'Ctrl+`' : 'Ctrl+` · host terminal unavailable', disabled: !terminalPanel.frame && (!state.online || !state.terminal) },
    { id: 'edit', label: 'Edit queued message', description: 'Ctrl+Shift+Enter', disabled: !state.online || !state.canEdit || ![...state.queue.values()].some(item => item.mode !== 'pending') },
    { id: 'approvals', label: 'Focus approvals', description: 'Ctrl+Shift+1', disabled: !oldest },
    { id: 'approve', label: 'Approve focused request', description: 'Ctrl+Shift+2', disabled: !state.online || !focused },
    { id: 'reject', label: 'Reject focused request', description: 'Ctrl+Shift+3', disabled: !state.online || !focused }
  ];
}
function promptAction(action) {
  if (action === 'terminal-unlock') {
    if (canUnlockPrompt()) sendPrompt(state.terminalUnlockCommand, 'send');
  } else if (action === 'focus') {
    editor.focus();
  } else if (action === 'approvals') {
    const item = [...state.queue.values()].find(candidate => candidate.held === '1');
    if (!item) { toast('No approvals are waiting.'); return; }
    state.approval = { id: item.id, revision: item.revision }; queue();
    [...$('queue').children].find(row => row.dataset.id === item.id)?.focus();
  } else if (action === 'approve' || action === 'reject') {
    decide(action, state.approval && state.queue.get(state.approval.id));
  } else if (action === 'edit') {
    editQueue(state.approval ? state.queue.get(state.approval.id) : [...state.queue.values()].filter(item => item.mode !== 'pending').at(-1));
  } else if (action === 'terminal') {
    if (terminalPanel.frame) terminalPanel.close();
    else if (state.online && state.terminal) terminalPanel.open(state.terminal);
    else toast('The host has not enabled a browser terminal.');
  }
}
$('queue-edit-cancel').addEventListener('click', () => { if (!state.editPending) $('queue-editor').close(); });
$('queue-editor').addEventListener('cancel', event => { if (state.editPending) event.preventDefault(); });
$('queue-editor').addEventListener('close', () => { state.edit = null; prompt.focus(); });
function saveEdit() {
  if (!state.edit || state.editPending) return;
  const text = $('queue-edit-text').value;
  if (text === state.edit.original) { $('queue-editor').close(); return; }
  if (!text.trim() || new TextEncoder().encode(text).length > 128 * 1024) { $('queue-edit-error').textContent = 'Use nonempty text within 128 KiB.'; return; }
  const request = randomId(), fields = { id: state.edit.id, revision: state.edit.revision, text, request, request_seq: ++state.requestSeq };
  if (send('edit', fields)) {
    state.editPending = request; state.pending.set(request, { action: 'edit', fields, text });
    $('queue-edit-save').disabled = $('queue-edit-cancel').disabled = $('queue-edit-text').disabled = true;
    $('queue-edit-error').textContent = 'Saving…';
  }
}
$('queue-edit-form').addEventListener('submit', event => { event.preventDefault(); saveEdit(); });
$('queue-edit-text').addEventListener('keydown', event => {
  if (event.key === 'Enter' && !event.shiftKey && !event.altKey && !event.ctrlKey && !event.metaKey && !event.isComposing) {
    event.preventDefault(); saveEdit();
  }
});
function editResult(request, error) {
  if (request !== state.editPending) return;
  state.editPending = null;
  $('queue-edit-save').disabled = $('queue-edit-cancel').disabled = $('queue-edit-text').disabled = false;
  if (error) $('queue-edit-error').textContent = error;
  else $('queue-editor').close();
}
// Keep in-flight image insertion points attached to edits, including the
// syntax-only removal of a completion trigger at the same insertion point.
function adjustAnchor(before, after, syntax) {
  if (syntax !== undefined) {
    for (const job of state.attachments) if (job.anchor > syntax) --job.anchor;
    return;
  }
  let start = 0, tail = 0;
  while (start < before.length && start < after.length && before[start] === after[start]) ++start;
  while (tail < before.length - start && tail < after.length - start && before[before.length - 1 - tail] === after[after.length - 1 - tail]) ++tail;
  const removed = before.length - tail - start, inserted = after.length - tail - start;
  for (const job of state.attachments) {
    if (removed && start <= job.anchor && job.anchor < start + removed) {
      job.cancelled = true;
      if (job === state.upload) cancelUpload('Image insertion point was removed.');
    } else if (start < job.anchor) job.anchor += inserted - removed;
  }
}
const editor = new Prompt({
  document, send, mutate: adjustAnchor, resize: resizePrompt, toast, submit,
  actions: actionItems, action: promptAction,
  lockedActions: () => canUnlockPrompt() ? [{ id: 'terminal-unlock', label: state.terminalUnlockCommand,
    description: 'Allow typing while terminals are open', disabled: !state.online }] : [],
  images: () => state.images, recovery: () => state.failed,
  recovered: item => { state.failed = state.failed.filter(candidate => candidate !== item); },
  imagePicker: () => $('image-input').click(),
  cancelImages: () => {
    if (!state.attachments.length || prompt.selectionStart !== prompt.selectionEnd) return false;
    cancelUploads('Image uploads cancelled.'); return true;
  },
  cancel: () => {
    if (state.approval) { state.approval = null; queue(); prompt.focus(); return; }
    if (state.attachments.length) cancelUploads('Image uploads cancelled.');
    else if (state.busy) send('cancel');
  }
});
window.addEventListener('focus', () => editor.focus(true));
const terminalPanel = new TerminalPanel({ document, window, closed: () => {
  state.restorePromptAfterTerminal = !editor.focus();
  if (state.restorePromptAfterTerminal) scroll.focus({ preventScroll: true });
} });
const transcripts = new Transcripts(document, window);
function promptState(disabled, text = '') {
  state.promptExplicitDisabled = disabled; state.promptReason = text;
  updatePromptState();
}
function canUnlockPrompt() {
  return !!state.terminalUnlockCommand && state.promptDisabled && !state.promptExplicitDisabled;
}
function updatePromptState() {
  const disabled = state.promptExplicitDisabled || (state.terminalPromptLock && state.terminalHandoffs > 0);
  const wasDisabled = state.promptDisabled;
  state.promptDisabled = disabled; prompt.readOnly = disabled;
  prompt.setAttribute('aria-disabled', String(disabled));
  prompt.title = disabled ? state.promptExplicitDisabled ? state.promptReason || 'The host has paused prompt input.' : 'A terminal handoff is active.' : '';
  $('composer').dataset.disabled = disabled ? '1' : '0';
  $('plus').disabled = disabled && !canUnlockPrompt();
  $('image-input').disabled = disabled;
  if (disabled && !wasDisabled) { stopSimulation(); editor.suspend(); }
  else if (!disabled && wasDisabled) {
    editor.hideMenu();
    if (state.online) editor.connected();
    editor.focus(!(state.restorePromptAfterTerminal && document.activeElement === scroll));
    state.restorePromptAfterTerminal = false;
  }
  if (disabled) editor.showMenu();
  resizePrompt();
}
function sendPrompt(text, action) {
  if (state.pending.size >= 32) {
    toast('Wait for the host to confirm pending prompts.');
    return false;
  }
  const request = randomId(), fields = { text, request, request_seq: ++state.requestSeq };
  if (!send(action, fields)) return false;
  state.pending.set(request, { action, fields, text });
  return true;
}
function stopSimulation() {
  if (simulation) {
    clearTimeout(simulation.timer);
    delete $('composer').dataset.simulating;
  }
  simulation = simulationInput = null;
}
function canSimulate() {
  return state.online && !state.promptDisabled && !state.attachments.length && !state.edit;
}
function simulationEvent(event) {
  if (event.type === 'simulate_begin') {
    simulationInput = null;
    const timing = [event.start, event.pace, event.submit];
    if (!timing.every(value => typeof value === 'string' && /^\d+$/u.test(value) &&
        Number(value) <= 2147483647) || !['q', 's'].includes(event.mode)) return;
    if (simulation || prompt.value.trim() || !canSimulate()) {
      toast('The prompt is busy. Simulated input was skipped.');
      return;
    }
    const [start, pace, after] = timing.map(Number);
    simulationInput = { start, pace, after, steer: event.mode === 's', chunks: [], length: 0 };
    return;
  }
  if (!simulationInput) return;
  if (event.type === 'simulate_text') {
    if (typeof event.text !== 'string' || (simulationInput.length += event.text.length) > 128 * 1024) {
      simulationInput = null;
      return;
    }
    simulationInput.chunks.push(event.text);
    return;
  }
  const job = simulationInput;
  simulationInput = null;
  const text = job.chunks.join('');
  if (!text.trim() || prompt.value.trim() || !canSimulate()) return;
  if (prompt.value) {
    prompt.value = '';
    prompt.setSelectionRange(0, 0);
    editor.reset();
  }
  job.chunks = [];
  simulation = job;
  $('composer').dataset.simulating = '1';
  editor.render();
  const segments = job.pace && (typeof Intl.Segmenter === 'function'
    ? new Intl.Segmenter().segment(text)[Symbol.iterator]() : text[Symbol.iterator]());
  let next = segments ? segments.next() : { value: text, done: false };
  function tick() {
    if (simulation !== job || !canSimulate()) { if (simulation === job) stopSimulation(); return; }
    const at = prompt.value.length;
    prompt.setRangeText(next.value.segment ?? next.value, at, at, 'end');
    if (at) editor.render();
    else editor.reset();
    if (prompt.scrollHeight > prompt.clientHeight) {
      prompt.scrollTop = prompt.scrollHeight;
      editor.syncScroll();
    }
    next = segments ? segments.next() : { done: true };
    if (!next.done) job.timer = setTimeout(tick, job.pace);
    else job.timer = setTimeout(() => editor.enqueue(() => {
      if (simulation !== job) return;
      stopSimulation();
      if (canSimulate() && prompt.value === text) submit(job.steer, true);
    }), job.after);
  }
  job.timer = setTimeout(tick, job.start);
}
prompt.addEventListener('beforeinput', stopSimulation);
prompt.addEventListener('input', stopSimulation);
prompt.addEventListener('compositionstart', stopSimulation);
prompt.addEventListener('keydown', event => {
  if (!simulation || ['Shift', 'Control', 'Alt', 'Meta'].includes(event.key)) return;
  stopSimulation();
  if (event.key !== 'Escape') return;
  event.preventDefault();
  event.stopImmediatePropagation();
  prompt.value = '';
  editor.reset();
}, true);
function submit(steer = false, automatic = false) {
  stopSimulation();
  if (state.promptDisabled || state.attachments.length) return;
  editor.finish();
  if (!prompt.value.trim()) return;
  if (new TextEncoder().encode(prompt.value).length > 128 * 1024) { toast('The prompt exceeds 128 KiB. Your draft is kept.'); return; }
  if (sendPrompt(prompt.value, steer ? 'steer' : 'send')) {
    prompt.value = ''; prompt.setSelectionRange(0, 0); started();
    if (!automatic) state.follow = true;
    editor.reset();
    if (!automatic) editor.focus();
  }
}
$('composer').addEventListener('submit', event => { event.preventDefault(); editor.enqueue(() => submit()); });
function credit(kind) {
  const upload = state.upload;
  if (!upload) return;
  if (upload.waiter) { upload.waiter.resolve(kind); upload.waiter = null; }
  else upload.credit = kind;
}
function waitCredit(upload) {
  if (upload.cancelled || state.upload !== upload) return Promise.reject(new Error('Image upload cancelled.'));
  if (upload.credit) { const kind = upload.credit; upload.credit = null; return Promise.resolve(kind); }
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => { upload.waiter = null; reject(new Error('Image upload timed out.')); }, 30000);
    upload.waiter = { resolve: value => { clearTimeout(timeout); resolve(value); }, reject: error => { clearTimeout(timeout); reject(error); } };
  });
}
function cancelUpload(reason) {
  const upload = state.upload;
  if (!upload) return;
  send('image_cancel', { id: upload.id }); upload.cancelled = true;
  upload.waiter?.reject(new Error(reason)); upload.reader?.cancel().catch(() => {});
  state.upload = null; upload.chip.remove(); URL.revokeObjectURL(upload.url);
  state.attachments = state.attachments.filter(job => job !== upload);
  $('uploads').hidden = !$('uploads').childElementCount; resizePrompt(); toast(reason);
}
function cancelUploads(reason) {
  for (const job of state.attachments) job.cancelled = true;
  state.attachments = []; uploadChain = Promise.resolve();
  if (state.upload) cancelUpload(reason); else toast(reason);
  resizePrompt();
}
async function uploadFile(upload) {
  const { file } = upload;
  if (upload.cancelled) return;
  if (file.size > state.imageLimit) throw new Error(`Image exceeds ${state.imageLimit / 1048576} MiB.`);
  const prefix = new Uint8Array(await file.slice(0, 8).arrayBuffer());
  const png = [137,80,78,71,13,10,26,10].every((b, i) => prefix[i] === b);
  const jpeg = prefix[0] === 255 && prefix[1] === 216 && prefix[2] === 255;
  if (!png && !jpeg) throw new Error('Only PNG and JPEG images are supported.');
  if (upload.cancelled) return;
  upload.url = URL.createObjectURL(file);
  state.upload = upload;
  const chip = document.createElement('div'), image = document.createElement('img'), label = document.createElement('span'), cancel = document.createElement('button');
  upload.chip = chip; chip.className = 'upload'; image.src = upload.url; image.alt = file.name || 'Image';
  previews.inspect(image, { url: upload.url, name: file.name, file, type: png ? 'image/png' : 'image/jpeg' });
  label.textContent = 'Uploading image…'; cancel.type = 'button'; cancel.textContent = '×'; cancel.setAttribute('aria-label', 'Cancel image upload');
  cancel.addEventListener('click', () => cancelUpload('Image upload cancelled.')); chip.append(image, label, cancel);
  $('uploads').append(chip); $('uploads').hidden = false; resizePrompt();
  if (!send('image_begin', { id: upload.id, media_type: png ? 'image/png' : 'image/jpeg', filename: file.name, size: file.size })) throw new Error('Disconnected.');
  await waitCredit(upload);
  upload.reader = file.stream().getReader();
  for (;;) {
    const { value, done } = await upload.reader.read();
    if (state.upload !== upload || upload.cancelled) throw new Error('Image upload cancelled.');
    if (done) break;
    for (let at = 0; at < value.byteLength; at += 8192) {
      if (state.upload !== upload || upload.cancelled || !state.online) throw new Error('Image upload cancelled.');
      state.socket.send(value.subarray(at, Math.min(at + 8192, value.byteLength)));
      await waitCredit(upload);
    }
  }
  label.textContent = 'Storing image…'; send('image_end', { id: upload.id });
  const result = await waitCredit(upload);
  if (typeof result !== 'object' || !result.path?.startsWith('/') || /[\u0000-\u001f\u007f]/u.test(result.path)) throw new Error('Server did not acknowledge a valid image path.');
  if (state.upload !== upload) throw new Error('Image upload cancelled.');
  editor.finish();
  const insertion = (upload.anchor && !/\s$/u.test(prompt.value.slice(0, upload.anchor)) ? ' ' : '') + result.path +
    (upload.anchor < prompt.value.length && !/^\s/u.test(prompt.value.slice(upload.anchor)) ? ' ' : '');
  prompt.setRangeText(insertion, upload.anchor, upload.anchor, 'preserve');
  previews.remember(result.path, upload.url, file.name, file, png ? 'image/png' : 'image/jpeg');
  upload.url = null;
  editor.reset();
  for (const job of state.attachments) if (job !== upload && job.anchor >= upload.anchor) job.anchor += insertion.length;
  state.upload = null; chip.remove();
  $('uploads').hidden = !$('uploads').childElementCount; resizePrompt(); editor.focus();
}
let uploadChain = Promise.resolve();
function addImages(files) {
  if (state.promptDisabled) return;
  if (!state.images) { toast('This host does not accept images.'); return; }
  editor.finish();
  for (const file of files) {
    if (state.attachments.length >= 16) { toast('Attach at most 16 images at a time.'); break; }
    const job = { file, id: randomId(), anchor: prompt.selectionStart };
    state.attachments.push(job); resizePrompt();
    uploadChain = uploadChain.then(() => uploadFile(job)).catch(error => {
      if (state.upload === job) cancelUpload(error.message); else if (!job.cancelled) toast(error.message);
    }).finally(() => { state.attachments = state.attachments.filter(item => item !== job); resizePrompt(); });
  }
}
$('image-input').addEventListener('change', () => { addImages($('image-input').files); $('image-input').value = ''; });
$('composer').addEventListener('dragover', event => { event.preventDefault(); $('composer').classList.add('dragging'); });
$('composer').addEventListener('dragleave', event => { if (!$('composer').contains(event.relatedTarget)) $('composer').classList.remove('dragging'); });
$('composer').addEventListener('drop', event => { event.preventDefault(); $('composer').classList.remove('dragging'); addImages(event.dataTransfer.files); });
prompt.addEventListener('paste', event => { const files = [...(event.clipboardData?.files || [])]; if (files.length) { event.preventDefault(); addImages(files); } });
function clearConversation() {
  transcripts.clear();
  previews.hide(); previews.viewer.close();
  for (const m of state.messages.values()) m.stream?.end();
  state.messages.clear(); messages.replaceChildren(); state.queue.clear(); state.stagingQueue = null;
  $('chat').classList.add('welcome-state');
  state.approval = null; state.busy = false; $('activity').textContent = ''; queue();
}
function receive(event) {
  const e = JSON.parse(event.data);
  if (e.seq) state.seq = e.seq;
  if (e.view && e.view !== state.view) return;
  switch (e.type) {
    case 'reset':
      clearConversation();
      if (e.text) toast(e.text); break;
    case 'begin': message(e.id, e.role); break;
    case 'delta': {
      const m = message(e.id, e.role);
      const text = e.encoding === 'base64'
        ? m.decoder.decode(Uint8Array.from(atob(e.data || ''), c => c.charCodeAt(0)), { stream: true })
        : e.text || '';
      m.raw.push(text);
      if (m.stream) m.stream.write(text); else previews.append(m.body, text, m);
      follow(); break;
    }
    case 'end': {
      const m = state.messages.get(e.id);
      if (m) {
        const tail = m.decoder.decode();
        if (tail) m.raw.push(tail);
        if (m.stream) { m.stream.write(tail); m.stream.end(); }
        else previews.append(m.body, tail, m, true);
      }
      follow(); break;
    }
    case 'queue_begin': state.stagingQueue = new Map(); break;
    case 'queue_item':
      state.stagingQueue?.set(e.id, { ...e, text: e.text || '', decoder: new TextDecoder() }); break;
    case 'queue_text': {
      const item = state.stagingQueue?.get(e.id);
      if (item) item.text += e.encoding === 'base64' ? item.decoder.decode(Uint8Array.from(atob(e.data || ''), c => c.charCodeAt(0)), { stream: true }) : e.text || '';
      break;
    }
    case 'queue_end': {
      if (!state.stagingQueue) break;
      const visibleAt = $('queue').hidden ? performance.now() + state.queueDelay : 0;
      for (const item of state.stagingQueue.values()) {
        item.text += item.decoder.decode();
        item.visibleAt = state.queue.get(item.id)?.visibleAt ?? (item.held === '1' ? 0 : visibleAt);
      }
      state.queue = state.stagingQueue; state.stagingQueue = null; queue(); break;
    }
    case 'status': state.busy = e.busy === '1'; $('activity').textContent = e.text || ''; $('activity').title = e.text || ''; break;
    case 'prompt_state': promptState(e.disabled === '1', e.text); break;
    case 'simulate_begin': case 'simulate_text': case 'simulate_end':
      simulationEvent(e); break;
    case 'terminal_state': {
      if (typeof e.active !== 'string' || !/^(0|[1-9]\d*)$/u.test(e.active) || !Number.isSafeInteger(Number(e.active)) || !['0', '1'].includes(e.disable_prompt)) break;
      state.terminalHandoffs = Number(e.active); state.terminalPromptLock = e.disable_prompt === '1';
      updatePromptState(); break;
    }
    case 'transcript_open': case 'transcript_data': case 'transcript_resize':
    case 'transcript_reset': case 'transcript_remove': transcripts.receive(e); break;
    case 'limits': if (Number(e.image_bytes) > 0) state.imageLimit = Number(e.image_bytes); break;
    case 'completion_provider': case 'complete': case 'complete_end': editor.receive(e); break;
    case 'draft': {
      stopSimulation();
      const before = prompt.value; prompt.value = e.text || ''; adjustAnchor(before, prompt.value); editor.reset(); prompt.focus(); break;
    }
    case 'image_credit': if (e.id === state.upload?.id) credit('ready'); break;
    case 'image_done': if (e.id === state.upload?.id) credit({ path: e.path }); break;
    case 'image_error': if (e.id === state.upload?.id) cancelUpload(e.text || 'Image upload failed.'); break;
    case 'session':
      stopSimulation();
      editor.newSession();
      state.terminalUnlockCommand = '';
      if (state.epoch && state.epoch !== e.id) {
        terminalPanel.close();
        state.terminalHandoffs = 0; state.terminalPromptLock = false;
        promptState(false);
        state.seq = ''; clearConversation();
        editResult(state.editPending, 'The host restarted. This edit was not sent again.');
        state.edit = null; $('queue-edit-save').disabled = true;
        state.canEdit = false; state.terminal = '';
      }
      state.online = true; $('connection').textContent = 'Connected';
      updatePromptState(); queue();
      if (!state.epoch || state.epoch === e.id) for (const item of state.pending.values()) send(item.action, item.fields);
      else if (state.epoch && state.pending.size) {
        state.failed.push(...state.pending.values()); state.failed = state.failed.slice(-16); state.pending.clear();
        toast('The host restarted. Unconfirmed prompts are available under +; they were not sent again.');
      }
      state.epoch = e.id; break;
    case 'capabilities': {
      state.canEdit = e.edit === '1';
      const command = e.terminal_unlock_command;
      state.terminalUnlockCommand = typeof command === 'string' && command.trim() &&
        !/[\u0000-\u001f\u007f\u2028\u2029]/u.test(command) ? command : '';
      updatePromptState();
      state.terminal = typeof e.terminal === 'string' && /^\/(?!\/)/u.test(e.terminal) && !/[\\\u0000-\u0020]/u.test(e.terminal) ? e.terminal : '';
      if (!state.terminal) terminalPanel.close();
      queue(); break;
    }
    case 'accepted': state.pending.delete(e.request); editResult(e.request); break;
    case 'error':
      editResult(e.request, e.text || 'The entry changed or was consumed. Your edit is kept.');
      if (state.pending.has(e.request)) { state.failed.push(state.pending.get(e.request)); state.failed = state.failed.slice(-16); state.pending.delete(e.request); }
      toast(e.text || 'The action failed. Unsent prompts can be recovered under +.'); break;
  }
}
function connect() {
  const generation = ++state.generation;
  const url = new URL('./events', document.baseURI || location.href); url.protocol = url.protocol === 'https:' ? 'wss:' : 'ws:';
  url.searchParams.set('after', state.seq); url.searchParams.set('view', state.view);
  const socket = state.socket = new WebSocket(url);
  socket.addEventListener('open', () => { $('connection').textContent = 'Synchronizing'; });
  socket.addEventListener('message', event => { try { receive(event); } catch { toast('Invalid conversation event. Reconnecting…'); socket.close(); } });
  socket.addEventListener('close', () => {
    if (generation !== state.generation) return;
    state.online = false; $('connection').textContent = 'Reconnecting'; resizePrompt(); queue();
    stopSimulation();
    editor.disconnected();
    if (state.attachments.length) cancelUploads('Disconnected. Image uploads cancelled; draft kept.');
    setTimeout(connect, 1000);
  });
}
try {
  const response = await fetch('./config.json');
  if (!response.ok) throw new Error('The web handler is not enabled.');
  const config = await response.json(); document.title = config.title || 'softline';
  state.images = config.images === '1';
  if (Number(config.image_bytes) > 0) state.imageLimit = Number(config.image_bytes);
  if (Number(config.queue_delay) >= 0) state.queueDelay = Number(config.queue_delay);
  $('welcome-text').replaceChildren(); const welcome = new MarkdownStream($('welcome-text')); welcome.write(config.welcome || 'Welcome to softline'); welcome.end();
  // This is host-supplied branding, separate from model Markdown.
  $('banner').innerHTML = config.banner || '';
  $('brand').innerHTML = config.header || '';
  $('brand').hidden = !config.header?.trim();
  connect();
} catch (error) { $('connection').textContent = 'Unavailable'; toast(error.message); }
resizePrompt();
