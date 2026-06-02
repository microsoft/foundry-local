// <complete_code>
// <imports>
import { ChatSession, FoundryLocalManager, Item, Request } from 'foundry-local-sdk';
// </imports>

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
// EP packages include dependencies and may be large.
// Download is only required again if a new version of the EP is released.
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
// Get the model object
const modelAlias = 'qwen2.5-0.5b'; // Using an available model from the list above
const model = await manager.catalog.getModel(modelAlias);

// Download the model
console.log(`\nDownloading model ${modelAlias}...`);
await model.download((progress) => {
    process.stdout.write(`\rDownloading... ${progress.toFixed(2)}%`);
});
console.log('\n✓ Model downloaded');

// Load the model
console.log(`\nLoading model ${modelAlias}...`);
await model.load();
console.log('✓ Model loaded');
// </model_setup>

// <chat_completion>
const session = new ChatSession(model);
try {
    const req1 = new Request();
    req1.addItem(Item.systemMessage('You are a concise science tutor. Answer in 1-2 sentences.'));
    req1.addItem(Item.userMessage('Why is the sky blue?'));

    console.log('Turn 1 (non-streaming):');
    const resp = await session.processRequest(req1);
    // Chat responses contain a single MessageItem; the native layer surfaces its
    // single TextItem part as `.content`.
    console.log(resp.output[0].content);

    // Session retains history — only the new turn is sent.
    const req2 = new Request();
    req2.addItem(Item.userMessage('And why are sunsets red?'));

    console.log('\nTurn 2 (streaming):');
    for await (const item of session.processStreamingRequest(req2)) {
        if (item.type === 'text') {
            process.stdout.write(item.text);
        }
    }
    process.stdout.write('\n');

    console.log(`\nCompleted turns in session: ${session.turnCount}`);
} finally {
    session.dispose();
}
// </chat_completion>

// <cleanup>
console.log('Unloading model...');
await model.unload();
console.log(`✓ Model unloaded`);
// </cleanup>
// </complete_code>
    