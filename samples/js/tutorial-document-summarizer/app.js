// Tutorial: Document Summarizer — Foundry Local JS SDK (native session API).
//
// Summarizes a text file (or every .txt in a directory). Each file gets a fresh
// ChatSession so prior documents don't leak into context.

import { ChatSession, FoundryLocalManager, Item, Request } from 'foundry-local-sdk';
import { readFileSync, readdirSync, statSync } from 'fs';
import { join, basename, dirname } from 'path';
import { fileURLToPath } from 'url';

const __dirname = dirname(fileURLToPath(import.meta.url));

const SYSTEM_PROMPT =
    'Summarize the following document into concise bullet points. ' +
    'Focus on the key points and main ideas.';

async function summarizeFile(model, filePath) {
    const content = readFileSync(filePath, 'utf-8');
    const session = new ChatSession(model);
    try {
        const request = new Request();
        request.addItem(Item.systemMessage(SYSTEM_PROMPT));
        request.addItem(Item.userMessage(content));

        for await (const item of session.processStreamingRequest(request)) {
            if (item.type === 'text' && item.text) {
                process.stdout.write(item.text);
            }
        }
        console.log();
    } finally {
        session.dispose();
    }
}

async function summarizeDirectory(model, directory) {
    const txtFiles = readdirSync(directory).filter((f) => f.endsWith('.txt')).sort();
    if (txtFiles.length === 0) {
        console.log(`No .txt files found in ${directory}`);
        return;
    }

    for (const fileName of txtFiles) {
        console.log(`--- ${fileName} ---`);
        await summarizeFile(model, join(directory, fileName));
        console.log();
    }
}

const manager = FoundryLocalManager.create({
    appName: 'foundry_local_samples',
    logLevel: 'info',
});

let currentEp = '';
await manager.downloadAndRegisterEps((epName, percent) => {
    if (epName !== currentEp) {
        if (currentEp !== '') process.stdout.write('\n');
        currentEp = epName;
    }
    process.stdout.write(`\r  ${epName.padEnd(30)}  ${percent.toFixed(1).padStart(5)}%`);
});
if (currentEp !== '') process.stdout.write('\n');

const model = await manager.catalog.getModel('qwen2.5-0.5b');

await model.download((progress) => {
    process.stdout.write(`\rDownloading model: ${progress.toFixed(2)}%`);
});
console.log('\nModel downloaded.');

await model.load();
console.log('Model loaded and ready.\n');

// Default to the shared samples/testdata/document.txt.
const defaultDocument = join(__dirname, '..', '..', 'testdata', 'document.txt');
const target = process.argv[2] || defaultDocument;

try {
    const stats = statSync(target);
    if (stats.isDirectory()) {
        await summarizeDirectory(model, target);
    } else {
        console.log(`--- ${basename(target)} ---`);
        await summarizeFile(model, target);
    }
} catch {
    console.log(`--- ${basename(target)} ---`);
    await summarizeFile(model, target);
}

await model.unload();
console.log('\nModel unloaded. Done!');
