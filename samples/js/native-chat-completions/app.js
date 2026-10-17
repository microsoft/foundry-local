// <complete_code>
// <imports>
import { ChatSession, FoundryLocalManager, Item, Request } from 'foundry-local-sdk';
// </imports>

function collectText(items) {
    const parts = [];
    for (const item of items) {
        if (item.type === 'text') {
            if ((item.textType ?? 'default') === 'default') {
                parts.push(item.text);
            }
            continue;
        }

        if (item.type === 'message') {
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

// Initialize the Foundry Local SDK
console.log('Initializing Foundry Local SDK...');

// <init>
const manager = FoundryLocalManager.create({
    appName: 'foundry_local_samples',
    logLevel: 'info'
});
// </init>
console.log('✓ SDK initialized successfully');

// Discover available execution providers and their registration status.
const eps = manager.discoverEps();
const maxNameLen = 30;
console.log('\nAvailable execution providers:');
console.log(`  ${'Name'.padEnd(maxNameLen)}  Registered`);
console.log(`  ${'─'.repeat(maxNameLen)}  ──────────`);
for (const ep of eps) {
    console.log(`  ${ep.name.padEnd(maxNameLen)}  ${ep.isRegistered}`);
}

// Download and register all execution providers with per-EP progress.
console.log('\nDownloading execution providers:');
if (eps.length > 0) {
    let currentEp = '';
    await manager.downloadAndRegisterEps((epName, percent) => {
        if (epName !== currentEp) {
            if (currentEp !== '') {
                process.stdout.write('\n');
            }
            currentEp = epName;
        }
        process.stdout.write(`\r  ${epName.padEnd(maxNameLen)}  ${percent.toFixed(1).padStart(5)}%`);
    });
    process.stdout.write('\n');
} else {
    console.log('No execution providers to download.');
}

// <model_setup>
const modelAlias = 'qwen2.5-0.5b';
const model = await manager.catalog.getModel(modelAlias);

console.log(`\nDownloading model ${modelAlias}...`);
await model.download((progress) => {
    process.stdout.write(`\rDownloading... ${progress.toFixed(2)}%`);
});
console.log('\n✓ Model downloaded');

console.log(`\nLoading model ${modelAlias}...`);
await model.load();
console.log('✓ Model loaded');
// </model_setup>

const session = new ChatSession(model);
session.setOptions({
    search: {
        temperature: 0.7,
        maxOutputTokens: 256
    }
});

try {
    // <chat_completion>
    console.log('\nCreating chat session...');
    console.log('✓ Chat session created');

    console.log('\nTesting chat completion...');
    const completion = await session.processRequest(
        new Request()
            .addItem(Item.systemMessage('You are a helpful assistant.'))
            .addItem(Item.userMessage('Why is the sky blue?'))
    );

    console.log('\nChat completion result:');
    console.log(collectText(completion.output));
    // </chat_completion>

    // <streaming>
    console.log('\nTesting streaming completion...');
    const stream = session.processStreamingRequest(
        new Request().addItem(Item.userMessage('Write a short poem about programming.'))
    );

    for await (const item of stream) {
        const text = collectText([item]);
        if (text) {
            process.stdout.write(text);
        }
    }
    console.log('\n');
    // </streaming>
} finally {
    session.dispose();

    // <cleanup>
    console.log('Unloading model...');
    await model.unload();
    console.log('✓ Model unloaded');
    // </cleanup>
}
// </complete_code>
