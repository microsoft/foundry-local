// Live Audio Transcription Example — Foundry Local JS SDK
//
// Demonstrates real-time microphone-to-text using the native v2 AudioSession API.
// Requires: npm install foundry-local-sdk naudiodon2
//
// Usage: node app.js

import { AudioSession, FoundryLocalManager, Item, ItemQueue, Request } from 'foundry-local-sdk';

console.log('╔══════════════════════════════════════════════════════════╗');
console.log('║   Foundry Local — Live Audio Transcription (JS SDK)      ║');
console.log('╚══════════════════════════════════════════════════════════╝');
console.log();

console.log('Initializing Foundry Local SDK...');
const manager = FoundryLocalManager.create({
    appName: 'foundry_local_samples',
    logLevel: 'info'
});
console.log('✓ SDK initialized');

let currentEp = '';
await manager.downloadAndRegisterEps((epName, percent) => {
    if (epName !== currentEp) {
        if (currentEp !== '') process.stdout.write('\n');
        currentEp = epName;
    }
    process.stdout.write(`\r  ${epName.padEnd(30)}  ${percent.toFixed(1).padStart(5)}%`);
});
if (currentEp !== '') process.stdout.write('\n');

const modelAlias = 'nemotron-speech-streaming-en-0.6b';
let model = await manager.catalog.getModel(modelAlias);

console.log(`Found model: ${model.id}`);
console.log('Downloading model (if needed)...');
await model.download((progress) => {
    process.stdout.write(`\rDownloading... ${progress.toFixed(2)}%`);
});
console.log('\n✓ Model downloaded');

console.log('Loading model...');
await model.load();
console.log('✓ Model loaded');

const session = new AudioSession(model);
const queue = new ItemQueue();

const request = new Request()
    .addItem(Item.audioDescriptor('pcm', 16000, 1))
    .addItem(queue)
    .setOptions({ additionalOptions: { language: 'en' } });

const stream = session.processStreamingRequest(request);

const readPromise = (async () => {
    try {
        for await (const item of stream) {
            if (item.type !== 'speechSegment' || !item.text) {
                continue;
            }

            if (item.kind === 'final') {
                process.stdout.write(`\n  [FINAL] ${item.text}\n`);
            } else if (item.kind === 'partial') {
                process.stdout.write(`\r${item.text}`);
            } else {
                process.stdout.write(item.text);
            }
        }

        const final = await stream.response;
        const result = final.output.find((item) => item.type === 'speechResult');
        if (!result) {
            throw new Error('Expected a speechResult item in the audio response.');
        }
        process.stdout.write(`\n[aggregate] ${result.text}\n`);
    } catch (err) {
        console.error('Stream error:', err.message);
        throw err;
    }
})();
// Observe failures immediately, even while microphone capture is still running.
readPromise.catch(() => {
    audioInput?.quit();
    process.exitCode = 1;
    void stopSession().catch((err) => console.error('Cleanup failed:', err.message));
});

let audioInput;
let stopped = false;

async function stopSession() {
    if (stopped) return;
    stopped = true;
    queue.markFinished();
    try {
        await readPromise;
    } finally {
        session.dispose();
        queue.dispose();
        await model.unload();
    }
    console.log('✓ Done');
}

try {
    const { default: portAudio } = await import('naudiodon2');

    audioInput = portAudio.AudioIO({
        inOptions: {
            channelCount: 1,
            sampleFormat: portAudio.SampleFormat16Bit,
            sampleRate: 16000,
            framesPerBuffer: 3200,
            maxQueue: 64
        }
    });

    audioInput.on('data', (buffer) => {
        const copy = new Uint8Array(buffer.buffer, buffer.byteOffset, buffer.byteLength).slice();
        queue.push(Item.bytes(copy));
    });

    console.log();
    console.log('════════════════════════════════════════════════════════════');
    console.log('  LIVE TRANSCRIPTION ACTIVE');
    console.log('  Speak into your microphone.');
    console.log('  Press Ctrl+C to stop.');
    console.log('════════════════════════════════════════════════════════════');
    console.log();

    audioInput.start();
} catch (err) {
    console.warn('⚠ Could not initialize microphone (naudiodon2 may not be installed).');
    console.warn('  Install with: npm install naudiodon2');
    console.warn('  Falling back to synthetic audio test...');
    console.warn();

    const sampleRate = 16000;
    const duration = 2;
    const totalSamples = sampleRate * duration;
    const pcmBytes = new Uint8Array(totalSamples * 2);
    for (let i = 0; i < totalSamples; i++) {
        const t = i / sampleRate;
        const sample = Math.round(32767 * 0.5 * Math.sin(2 * Math.PI * 440 * t));
        pcmBytes[i * 2] = sample & 0xFF;
        pcmBytes[i * 2 + 1] = (sample >> 8) & 0xFF;
    }

    const chunkSize = (sampleRate / 10) * 2;
    for (let offset = 0; offset < pcmBytes.length; offset += chunkSize) {
        const end = Math.min(offset + chunkSize, pcmBytes.length);
        queue.push(Item.bytes(pcmBytes.slice(offset, end)));
        await new Promise((resolve) => setTimeout(resolve, 100));
    }

    console.log('✓ Synthetic audio pushed');
    await new Promise((resolve) => setTimeout(resolve, 3000));
    await stopSession();
}

process.on('SIGINT', async () => {
    console.log('\n\nStopping...');
    if (audioInput) {
        audioInput.quit();
    }
    try {
        await stopSession();
    } catch (err) {
        console.error('Transcription failed:', err.message);
        process.exitCode = 1;
    }
});
