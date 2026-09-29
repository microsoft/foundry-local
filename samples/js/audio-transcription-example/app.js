// <complete_code>
// <imports>
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { FoundryLocalManager } from 'foundry-local-sdk';
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
// Create audio client
console.log('\nCreating audio client...');
const audioClient = model.createAudioClient();
console.log('✓ Audio client created');

// Example audio transcription
const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const defaultAudioFile = path.resolve(scriptDir, '../../assets/audio/Recording.mp3');
const audioFile = process.argv[2] || defaultAudioFile;
console.log(`\nTranscribing ${audioFile}...`);
const transcription = await audioClient.transcribe(audioFile);

console.log('\nAudio transcription result:');
console.log(transcription.text);
console.log('✓ Audio transcription completed');

// Same example but with streaming transcription using async iteration
console.log('\nTesting streaming audio transcription...');
for await (const result of audioClient.transcribeStreaming(audioFile)) {
    // Output the intermediate transcription results as they are received without line ending
    process.stdout.write(result.text);
}
console.log('\n✓ Streaming transcription completed');
// </transcription>

// <cleanup>
// Unload the model
console.log('Unloading model...');
await model.unload();
console.log(`✓ Model unloaded`);
// </cleanup>
// </complete_code>
