// <complete_code>
// <imports>
import { AudioSession, FoundryLocalManager, Item, Request } from 'foundry-local-sdk';
// </imports>

function findSpeechResult(items) {
    return items.find((item) => item.type === 'speechResult') ?? null;
}

console.log('Initializing Foundry Local SDK...');

// <init>
const manager = FoundryLocalManager.create({
    appName: 'foundry_local_samples',
    logLevel: 'info'
});
// </init>
console.log('✓ SDK initialized successfully');

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
const modelAlias = 'whisper-tiny';
const model = await manager.catalog.getModel(modelAlias);
console.log(`Using model: ${model.id}`);

console.log(`\nDownloading model ${modelAlias}...`);
await model.download((progress) => {
    process.stdout.write(`\rDownloading... ${progress.toFixed(2)}%`);
});
console.log('\n✓ Model downloaded');

console.log(`\nLoading model ${modelAlias}...`);
await model.load();
console.log('✓ Model loaded');
// </model_setup>

const session = new AudioSession(model);

try {
    // <transcription>
    console.log('\nCreating audio session...');
    console.log('✓ Audio session created');

    const audioFile = process.argv[2] || './Recording.mp3';

    console.log(`\nTranscribing ${audioFile}...`);
    const request = new Request()
        .addItem(Item.audioFromUri(audioFile))
        .setOptions({ additionalOptions: { language: 'en' } });

    const transcription = await session.processRequest(request);
    const result = findSpeechResult(transcription.output);
    if (!result) {
        throw new Error('Expected a speechResult item in the transcription response.');
    }

    console.log('\nAudio transcription result:');
    console.log(result.text);
    console.log('✓ Audio transcription completed');

    console.log('\nTesting streaming audio transcription...');
    const stream = session.processStreamingRequest(
        new Request()
            .addItem(Item.audioFromUri(audioFile))
            .setOptions({ additionalOptions: { language: 'en' } })
    );

    for await (const item of stream) {
        if (item.type === 'speechSegment') {
            process.stdout.write(item.text);
        }
    }
    console.log('\n✓ Streaming transcription completed');
    // </transcription>
} finally {
    session.dispose();

    // <cleanup>
    console.log('Unloading model...');
    await model.unload();
    console.log('✓ Model unloaded');
    // </cleanup>
}
// </complete_code>
