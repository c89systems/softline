import { readFile } from 'node:fs/promises';
import { runWebCompletionTests } from './web-completion.mjs';
const source = await readFile(new URL('../web/softline.js', import.meta.url), 'utf8');
for (const name of await runWebCompletionTests(source)) console.log(`PASS ${name}`);
