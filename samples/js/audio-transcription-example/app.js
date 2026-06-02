// <complete_code>
// <imports>
import { AudioSession, FoundryLocalManager, Item, Request } from 'foundry-local-sdk';
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

// Download and register all execution providers.
let currentEp = '';
await manager.downloadAndRegisterEps((epName, percent) => {
    if (epName !== currentEp) {
        if (currentEp !== '') process.stdout.write('\n');
        currentEp = epName;
    }
    process.stdout.write(`\r  ${epName.padEnd(30)}  ${percent.toFixed(1).padStart(5)}%`);
});
if (currentEp !== '') process.stdout.write('\n');

// <model_setup>
// Get the model object
const modelAlias = 'whisper-tiny'; // Using an available model from the list above
let model = await manager.catalog.getModel(modelAlias);
console.log(`Using model: ${model.id}`);

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

// <transcription>
const audioFile = process.argv[2] || './Recording.mp3';
console.log(`Transcribing audio with streaming output: ${audioFile}`);

const session = new AudioSession(model);
try {
    session.setOptions({ additionalOptions: { language: 'en' } });
    const req = new Request();
    req.addItem(Item.audioFromUri(audioFile));
    for await (const item of session.processStreamingRequest(req)) {
        if (item.type === 'text') {
            process.stdout.write(item.text);
        }
    }
    process.stdout.write('\n');
} finally {
    session.dispose();
}
// </transcription>

// <cleanup>
// Unload the model
console.log('Unloading model...');
await model.unload();
console.log(`✓ Model unloaded`);
// </cleanup>
// </complete_code>
