// Tutorial: Chat Assistant — Foundry Local JS SDK (native session API).
//
// Multi-turn streaming chat using ChatSession. The session retains conversation
// history; we only add the new user turn each iteration.

import { ChatSession, FoundryLocalManager, Item, Request } from 'foundry-local-sdk';
import * as readline from 'readline';

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
console.log('Model loaded and ready.');

const session = new ChatSession(model);

try {
    const rl = readline.createInterface({ input: process.stdin, output: process.stdout });
    const askQuestion = (prompt) => new Promise((resolve) => rl.question(prompt, resolve));

    console.log("\nChat assistant ready! Type 'quit' to exit.\n");

    let firstTurn = true;
    while (true) {
        const userInput = await askQuestion('You: ');
        const trimmed = userInput.trim().toLowerCase();
        if (trimmed === 'quit' || trimmed === 'exit') {
            break;
        }

        const request = new Request();
        if (firstTurn) {
            request.addItem(Item.systemMessage(
                'You are a helpful, friendly assistant. Keep your responses concise and ' +
                "conversational. If you don't know something, say so."
            ));
            firstTurn = false;
        }
        request.addItem(Item.userMessage(userInput));

        process.stdout.write('Assistant: ');
        for await (const item of session.processStreamingRequest(request)) {
            if (item.type === 'text' && item.text) {
                process.stdout.write(item.text);
            }
        }
        console.log('\n');
    }

    rl.close();
} finally {
    session.dispose();
}

await model.unload();
console.log('Model unloaded. Goodbye!');
