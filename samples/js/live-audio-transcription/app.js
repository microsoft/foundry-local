// Live Audio Transcription Example — Foundry Local JS SDK
//
// Demonstrates real-time microphone-to-text using the JS SDK.
// Requires: npm install foundry-local-sdk naudiodon2
//
// Usage: node app.js

import { FoundryLocalManager } from 'foundry-local-sdk';

console.log('╔══════════════════════════════════════════════════════════╗');
console.log('║   Foundry Local — Live Audio Transcription (JS SDK)      ║');
console.log('╚══════════════════════════════════════════════════════════╝');
console.log();

// Initialize the Foundry Local SDK
console.log('Initializing Foundry Local SDK...');
const manager = FoundryLocalManager.create({
    appName: 'foundry_local_samples',
    logLevel: 'info'
});
console.log('✓ SDK initialized');

// Get and load the nemotron model
// English-only:
const modelAlias = 'nemotron-speech-streaming-en-0.6b';
// Multi-lingual (supports 30+ languages including auto-detect):
// const modelAlias = 'nemotron-3.5-asr-streaming-0.6b';
let model = await manager.catalog.getModel(modelAlias);
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

// Create live transcription session (same pattern as C# sample).
const audioClient = model.createAudioClient();
const session = audioClient.createLiveTranscriptionSession();

session.settings.sampleRate = 16000;  // Default is 16000; shown here for clarity
session.settings.channels = 1;
session.settings.bitsPerSample = 16;
session.settings.language = 'en';                  // English (default)
// Multi-lingual examples:
// session.settings.language = 'de';     // German
// session.settings.language = 'zh-CN';  // Chinese (Simplified)
// session.settings.language = 'auto';   // Auto-detect language

console.log('Starting streaming session...');
await session.start();
console.log('✓ Session started');

// Read transcription results in background
const readPromise = (async () => {
    try {
        for await (const result of session.getStream()) {
            const text = result.content?.[0]?.text;
            if (!text) continue;

            // `is_final` is a transcript-state marker only. It should not stop the app.
            if (result.is_final) {
                process.stdout.write(`\n  [FINAL] ${text}\n`);
            } else {
                process.stdout.write(text);
            }
        }
    } catch (err) {
        if (err.name !== 'AbortError') {
            console.error('Stream error:', err.message);
        }
    }
})();

// --- Microphone capture ---
// This example uses naudiodon2 for cross-platform audio capture.
// Install with: npm install naudiodon2
//
// If you prefer a different audio library, just push PCM bytes
// (16-bit signed LE, mono, 16kHz) via session.append().

let audioInput;
try {
    const { default: portAudio } = await import('naudiodon2');

    audioInput = portAudio.AudioIO({
        inOptions: {
            channelCount: session.settings.channels,
            sampleFormat: session.settings.bitsPerSample === 16
                ? portAudio.SampleFormat16Bit
                : portAudio.SampleFormat32Bit,
            sampleRate: session.settings.sampleRate,
            // Larger chunk size lowers callback frequency and reduces overflow risk.
            framesPerBuffer: 3200,
            // Allow deeper native queue during occasional event-loop stalls.
            maxQueue: 64
        }
    });

    const appendQueue = [];
    let pumping = false;
    let warnedQueueDrop = false;

    const pumpAudio = async () => {
        if (pumping) return;
        pumping = true;
        try {
            while (appendQueue.length > 0) {
                const pcm = appendQueue.shift();
                await session.append(pcm);
            }
        } catch (err) {
            console.error('append error:', err.message);
        } finally {
            pumping = false;
            // Handle race where new data arrived after loop exit.
            if (appendQueue.length > 0) {
                void pumpAudio();
            }
        }
    };

    audioInput.on('data', (buffer) => {
        // Single copy: slice the underlying ArrayBuffer to get an independent Uint8Array.
        const copy = new Uint8Array(buffer.buffer, buffer.byteOffset, buffer.byteLength).slice();

        // Keep a bounded queue to avoid unbounded memory growth.
        if (appendQueue.length >= 100) {
            appendQueue.shift();
            if (!warnedQueueDrop) {
                warnedQueueDrop = true;
                console.warn('Audio append queue overflow; dropping oldest chunk to keep stream alive.');
            }
        }

        appendQueue.push(copy);
        void pumpAudio();
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

    // Fallback: push 2 seconds of synthetic PCM (440Hz sine wave)
    const sampleRate = session.settings.sampleRate;
    const duration = 2;
    const totalSamples = sampleRate * duration;
    const pcmBytes = new Uint8Array(totalSamples * 2);
    for (let i = 0; i < totalSamples; i++) {
        const t = i / sampleRate;
        const sample = Math.round(32767 * 0.5 * Math.sin(2 * Math.PI * 440 * t));
        pcmBytes[i * 2] = sample & 0xFF;
        pcmBytes[i * 2 + 1] = (sample >> 8) & 0xFF;
    }

    // Push in 100ms chunks
    const chunkSize = (sampleRate / 10) * 2;
    for (let offset = 0; offset < pcmBytes.length; offset += chunkSize) {
        const len = Math.min(chunkSize, pcmBytes.length - offset);
        await session.append(pcmBytes.slice(offset, offset + len));
    }

    console.log('✓ Synthetic audio pushed');
    console.log('Waiting briefly for final transcription results...');
    await new Promise((resolve) => setTimeout(resolve, 3000));
    await session.stop();
    await readPromise;
    await session.dispose();
    await model.unload();
    console.log('✓ Done');
    process.exit(0);
}

// Handle graceful shutdown
process.on('SIGINT', async () => {
    console.log('\n\nStopping...');
    if (audioInput) {
        audioInput.quit();
    }
    await session.stop();
    await readPromise;
    await session.dispose();
    await model.unload();
    console.log('✓ Done');
    process.exit(0);
});
