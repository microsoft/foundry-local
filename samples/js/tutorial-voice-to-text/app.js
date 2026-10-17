// <complete_code>
// <imports>
import { AudioSession, ChatSession, FoundryLocalManager, Item, Request } from 'foundry-local-sdk';
import { fileURLToPath } from 'url';
import path from 'path';
// </imports>

const __dirname = path.dirname(fileURLToPath(import.meta.url));

function collectText(items) {
    const parts = [];
    for (const item of items) {
        if (item.type === 'text' && (item.textType ?? 'default') === 'default') {
            parts.push(item.text);
        } else if (item.type === 'message') {
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

function requireSpeechResult(response) {
    const result = response.output.find((item) => item.type === 'speechResult');
    if (!result) {
        throw new Error('Expected a speechResult item in the audio response.');
    }
    return result;
}

// <init>
const manager = FoundryLocalManager.create({
    appName: 'foundry_local_samples',
    logLevel: 'info'
});
// </init>

let currentEp = '';
await manager.downloadAndRegisterEps((epName, percent) => {
    if (epName !== currentEp) {
        if (currentEp !== '') process.stdout.write('\n');
        currentEp = epName;
    }
    process.stdout.write(`\r  ${epName.padEnd(30)}  ${percent.toFixed(1).padStart(5)}%`);
});
if (currentEp !== '') process.stdout.write('\n');

// <transcription>
const speechModel = await manager.catalog.getModel('whisper-tiny');
await speechModel.download((progress) => {
    process.stdout.write(`\rDownloading speech model: ${progress.toFixed(2)}%`);
});
console.log('\nSpeech model downloaded.');

await speechModel.load();
console.log('Speech model loaded.');

const audioSession = new AudioSession(speechModel);
const transcriptionResponse = await audioSession.processRequest(
    new Request()
        .addItem(Item.audioFromUri(path.join(__dirname, 'meeting-notes.wav')))
        .setOptions({ additionalOptions: { language: 'en' } })
);
const transcription = requireSpeechResult(transcriptionResponse).text;
console.log(`\nTranscription:\n${transcription}`);

audioSession.dispose();
await speechModel.unload();
// </transcription>

// <summarization>
const chatModel = await manager.catalog.getModel('qwen2.5-0.5b');
await chatModel.download((progress) => {
    process.stdout.write(`\rDownloading chat model: ${progress.toFixed(2)}%`);
});
console.log('\nChat model downloaded.');

await chatModel.load();
console.log('Chat model loaded.');

const chatSession = new ChatSession(chatModel);
chatSession.setOptions({
    search: {
        temperature: 0.7,
        maxOutputTokens: 512
    }
});

const summaryResponse = await chatSession.processRequest(
    new Request()
        .addItem(Item.systemMessage(
            'You are a note-taking assistant. Summarize the following transcription into organized, concise notes with bullet points.'
        ))
        .addItem(Item.userMessage(transcription))
);
const summary = collectText(summaryResponse.output);
console.log(`\nSummary:\n${summary}`);

chatSession.dispose();
await chatModel.unload();
console.log('\nDone. Models unloaded.');
// </summarization>
// </complete_code>
