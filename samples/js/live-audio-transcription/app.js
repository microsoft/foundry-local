// Live Audio Transcription Example — Foundry Local JS SDK (native session API).
//
// Uses AudioSession + ItemQueue + processStreamingRequest directly.
// Microphone capture uses naudiodon2 when available; otherwise the sample falls
// back to synthetic PCM audio (or pass `--synth` to force the fallback).
//
// Usage:
//   npm install
//   node app.js          # Live microphone (Press ENTER to stop)
//   node app.js --synth  # Force synthetic audio

import {
    AudioSession,
    FoundryLocalManager,
    Item,
    ItemQueue,
    Request,
} from 'foundry-local-sdk';

const SAMPLE_RATE = 16000;
const CHANNELS = 1;
const LANGUAGE = 'en'; // try 'de', 'zh-CN', or 'auto' with the multilingual model

console.log('╔══════════════════════════════════════════════════════════╗');
console.log('║   Foundry Local — Live Audio Transcription (JS SDK)      ║');
console.log('╚══════════════════════════════════════════════════════════╝');
console.log();

const useSynth = process.argv.includes('--synth');

console.log('Initializing Foundry Local SDK...');
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

// English-only:
const modelAlias = 'nemotron-speech-streaming-en-0.6b';
// Multi-lingual (supports 30+ languages including auto-detect):
// const modelAlias = 'nvidia-nemotron-3.5-asr-streaming-multilingual-0.6b';
const model = await manager.catalog.getModel(modelAlias);
if (!model) {
    console.error(`ERROR: Model "${modelAlias}" not found in catalog.`);
    process.exit(1);
}

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
try {
    session.setOptions({ additionalOptions: { language: LANGUAGE } });

    // ItemQueue stays caller-owned so we can keep pushing chunks while
    // processStreamingRequest is in flight.
    const audioQueue = new ItemQueue();
    const request = new Request();
    // 1) Audio format descriptor (no data) tells the session how to interpret subsequent chunks.
    request.addItem(Item.audioDescriptor('pcm', SAMPLE_RATE, CHANNELS));
    // 2) Streaming input queue.
    request.addItem(audioQueue);

    // Kick off streaming. Keep the StreamingResponse so we can await the
    // terminal Response (with the aggregated transcript) after draining.
    const stream = session.processStreamingRequest(request);

    // Background reader: print TextItems (in cyan) as they stream in.
    const readPromise = (async () => {
        try {
            for await (const item of stream) {
                if (item.type === 'text' && item.text) {
                    process.stdout.write(`\x1b[96m${item.text}\x1b[0m`);
                }
            }
        } catch (err) {
            console.error(`\n[reader error] ${err.message}`);
        }
    })();

    let capturedLive = false;
    if (!useSynth) {
        capturedLive = await tryCaptureMicrophone(audioQueue);
    }

    if (!capturedLive) {
        if (!useSynth) {
            console.log('Microphone capture unavailable. Falling back to synthetic audio...');
        }
        pushSyntheticAudio(audioQueue);
    }

    // Signal end-of-input; the streaming session drains remaining items and completes.
    audioQueue.markFinished();
    await readPromise;
    audioQueue.dispose();
    process.stdout.write('\n');

    // Terminal Response carries the aggregated transcription as a single TextItem.
    const finalResponse = await stream.response;
    const finalItem = finalResponse.output[0];
    const finalText = finalItem?.type === 'text' ? finalItem.text : '';
    console.log();
    console.log('════════════════════════════════════════════════════════════');
    console.log('  FINAL TRANSCRIPTION');
    console.log('════════════════════════════════════════════════════════════');
    console.log(finalText);
} finally {
    session.dispose();
}

await model.unload();
console.log('✓ Done');
process.exit(0);


async function tryCaptureMicrophone(audioQueue) {
    let portAudio;
    try {
        ({ default: portAudio } = await import('naudiodon2'));
    } catch {
        return false;
    }

    let audioInput;
    try {
        audioInput = portAudio.AudioIO({
            inOptions: {
                channelCount: CHANNELS,
                sampleFormat: portAudio.SampleFormat16Bit,
                sampleRate: SAMPLE_RATE,
                // Larger chunk size lowers callback frequency and reduces overflow risk.
                framesPerBuffer: 3200,
                // Allow deeper native queue during occasional event-loop stalls.
                maxQueue: 64,
            },
        });
    } catch (err) {
        console.error(`\n[mic init failed] ${err.message}`);
        return false;
    }

    let stopping = false;
    let warnedQueueDrop = false;

    audioInput.on('data', (buffer) => {
        if (stopping) return;

        // Single copy: detach from the underlying ArrayBuffer.
        const copy = new Uint8Array(buffer.buffer, buffer.byteOffset, buffer.byteLength).slice();

        // Bounded queue to avoid unbounded memory growth.
        if (audioQueue.size >= 100) {
            if (!warnedQueueDrop) {
                warnedQueueDrop = true;
                console.warn('\nAudio queue overflow; dropping chunk to keep stream alive.');
            }
            return;
        }
        audioQueue.push(Item.bytes(copy));
    });

    console.log();
    console.log('════════════════════════════════════════════════════════════');
    console.log('  LIVE TRANSCRIPTION ACTIVE');
    console.log('  Speak into your microphone.');
    console.log('  Transcription appears in real-time (cyan text).');
    console.log('  Press ENTER to stop recording.');
    console.log('════════════════════════════════════════════════════════════');
    console.log();

    audioInput.start();

    // Wait for ENTER on stdin (avoids killing parent test scripts via Ctrl+C).
    await new Promise((resolve) => {
        process.stdin.resume();
        process.stdin.once('data', resolve);
    });

    stopping = true;
    await new Promise((resolve) => audioInput.quit(resolve));
    process.stdin.pause();
    return true;
}

function pushSyntheticAudio(audioQueue) {
    console.log('Pushing synthetic audio (440 Hz sine, 2s)...');
    const duration = 2;
    const totalSamples = SAMPLE_RATE * duration;
    const pcmBytes = new Uint8Array(totalSamples * 2);
    for (let i = 0; i < totalSamples; i++) {
        const t = i / SAMPLE_RATE;
        const sample = Math.round(32767 * 0.5 * Math.sin(2 * Math.PI * 440 * t));
        pcmBytes[i * 2] = sample & 0xff;
        pcmBytes[i * 2 + 1] = (sample >> 8) & 0xff;
    }

    const chunkSize = (SAMPLE_RATE / 10) * 2; // 100 ms
    for (let offset = 0; offset < pcmBytes.length; offset += chunkSize) {
        const len = Math.min(chunkSize, pcmBytes.length - offset);
        audioQueue.push(Item.bytes(pcmBytes.slice(offset, offset + len)));
    }
}
