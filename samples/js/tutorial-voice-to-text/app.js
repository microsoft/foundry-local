// Tutorial: Voice to Text — Foundry Local JS SDK (native session API).
//
// Transcribes an audio file with AudioSession (whisper), then summarizes the
// transcription into organized notes with ChatSession (qwen).

import {
    AudioSession,
    ChatSession,
    FoundryLocalManager,
    Item,
    Request,
} from 'foundry-local-sdk';
import { fileURLToPath } from 'url';
import path from 'path';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

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

// --- Step 1: Transcription ---
const speechModel = await manager.catalog.getModel('whisper-tiny');
await speechModel.download((progress) => {
    process.stdout.write(`\rDownloading speech model: ${progress.toFixed(2)}%`);
});
console.log('\nSpeech model downloaded.');

await speechModel.load();
console.log('Speech model loaded.');

// Default to the shared samples/testdata/meeting-notes.wav.
const defaultAudio = path.join(__dirname, '..', '..', 'testdata', 'meeting-notes.wav');
const audioPath = path.resolve(process.argv[2] || defaultAudio);

let transcription = '';
const speechSession = new AudioSession(speechModel);
try {
    const request = new Request();
    request.addItem(Item.audioFromUri(audioPath));
    for await (const item of speechSession.processStreamingRequest(request)) {
        if (item.type === 'text' && item.text) {
            transcription += item.text;
        }
    }
} finally {
    speechSession.dispose();
}
console.log(`\nTranscription:\n${transcription}`);

await speechModel.unload();

// --- Step 2: Summarization ---
const chatModel = await manager.catalog.getModel('qwen2.5-0.5b');
await chatModel.download((progress) => {
    process.stdout.write(`\rDownloading chat model: ${progress.toFixed(2)}%`);
});
console.log('\nChat model downloaded.');

await chatModel.load();
console.log('Chat model loaded.');

const chatSession = new ChatSession(chatModel);
try {
    const request = new Request();
    request.addItem(Item.systemMessage(
        'You are a note-taking assistant. Summarize the following transcription into ' +
        'organized, concise notes with bullet points.'
    ));
    request.addItem(Item.userMessage(transcription));

    process.stdout.write('\nSummary:\n');
    for await (const item of chatSession.processStreamingRequest(request)) {
        if (item.type === 'text' && item.text) {
            process.stdout.write(item.text);
        }
    }
    console.log();
} finally {
    chatSession.dispose();
}

await chatModel.unload();
console.log('\nDone. Models unloaded.');
