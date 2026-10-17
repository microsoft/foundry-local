// <complete_code>
// <imports>
import { ChatSession, FoundryLocalManager, Item, Request } from 'foundry-local-sdk';
import { readFileSync, readdirSync, statSync } from 'fs';
import { join, basename } from 'path';
// </imports>

function collectText(items) {
    const parts = [];
    for (const item of items) {
        if (item.type === 'text' && (item.textType ?? 'default') === 'default') {
            parts.push(item.text);
        } else if (item.type === 'message') {
            if (Array.isArray(item.parts)) {
                for (const part of item.parts) {
                    if (part.type === 'text' && (part.textType ?? 'default') === 'default') {
                        parts.push(part.text);
                    }
                }
            } else if (typeof item.content === 'string') {
                parts.push(item.content);
            }
        }
    }
    return parts.join('');
}

async function summarizeFile(model, filePath, systemPrompt) {
    const content = readFileSync(filePath, 'utf-8');
    const session = new ChatSession(model);
    session.setOptions({
        search: {
            temperature: 0.7,
            maxOutputTokens: 512
        }
    });

    try {
        const response = await session.processRequest(
            new Request()
                .addItem(Item.systemMessage(systemPrompt))
                .addItem(Item.userMessage(content))
        );
        console.log(collectText(response.output));
    } finally {
        session.dispose();
    }
}

async function summarizeDirectory(model, directory, systemPrompt) {
    const txtFiles = readdirSync(directory)
        .filter((f) => f.endsWith('.txt'))
        .sort();

    if (txtFiles.length === 0) {
        console.log(`No .txt files found in ${directory}`);
        return;
    }

    for (const fileName of txtFiles) {
        console.log(`--- ${fileName} ---`);
        await summarizeFile(model, join(directory, fileName), systemPrompt);
        console.log();
    }
}

// <init>
const manager = FoundryLocalManager.create({
    appName: 'foundry_local_samples',
    logLevel: 'info'
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
// </init>

// <summarization>
const systemPrompt =
    'Summarize the following document into concise bullet points. ' +
    'Focus on the key points and main ideas.';

// <file_reading>
const target = process.argv[2] || 'document.txt';
// </file_reading>

try {
    const stats = statSync(target);
    if (stats.isDirectory()) {
        await summarizeDirectory(model, target, systemPrompt);
    } else {
        console.log(`--- ${basename(target)} ---`);
        await summarizeFile(model, target, systemPrompt);
    }
} finally {
    await model.unload();
}
// </summarization>

console.log('\nModel unloaded. Done!');
// </complete_code>
