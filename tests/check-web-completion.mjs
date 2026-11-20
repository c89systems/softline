import { readFile } from 'node:fs/promises';
import { runWebCompletionTests } from './web-completion.mjs';
const inputs = [];
for (const path of ['../web/softline.js', '../web/completion.js', '../web/prompt.js', './completion-parity.tsv', '../web/images.js', '../web/image-viewer.js', '../web/terminal-panel.js', '../web/transcripts.js', '../web/pane-divider.js'])
  inputs.push(await readFile(new URL(path, import.meta.url), 'utf8'));
for (const name of await runWebCompletionTests(...inputs)) console.log(`PASS ${name}`);
