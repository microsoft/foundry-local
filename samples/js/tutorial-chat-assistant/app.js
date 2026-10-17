// <complete_code>
// <imports>
import { ChatSession, FoundryLocalManager, Item, Request } from 'foundry-local-sdk';
import * as readline from 'readline';
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
console.log('Model loaded and ready.');

const session = new ChatSession(model);
session.setOptions({
    search: {
        temperature: 0.7,
        maxOutputTokens: 512
    }
});
// </init>

// <system_prompt>
const systemPrompt =
    'You are a helpful, friendly assistant. Keep your responses ' +
    'concise and conversational. If you don\'t know something, say so.';
// </system_prompt>

const rl = readline.createInterface({
    input: process.stdin,
    output: process.stdout
});

const askQuestion = (prompt) => new Promise((resolve) => rl.question(prompt, resolve));

console.log('\nChat assistant ready! Type \'quit\' to exit.\n');

let firstTurn = true;

try {
    // <conversation_loop>
    while (true) {
        const userInput = await askQuestion('You: ');
        if (userInput.trim().toLowerCase() === 'quit' ||
            userInput.trim().toLowerCase() === 'exit') {
            break;
        }

        const request = new Request();
        if (firstTurn) {
            request.addItem(Item.systemMessage(systemPrompt));
            firstTurn = false;
        }
        request.addItem(Item.userMessage(userInput));

        // <streaming>
        process.stdout.write('Assistant: ');
        const stream = session.processStreamingRequest(request);
        let fullResponse = '';
        for await (const item of stream) {
            const text = collectText([item]);
            if (text) {
                process.stdout.write(text);
                fullResponse += text;
            }
        }
        console.log('\n');
        // </streaming>
    }
    // </conversation_loop>
} finally {
    session.dispose();
    await model.unload();
    console.log('Model unloaded. Goodbye!');
    rl.close();
}
// </complete_code>
