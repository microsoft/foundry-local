// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
//
// chat-and-audio sample — Foundry Local JS SDK (native session API).
//
// 1. Transcribe an audio file with AudioSession (whisper).
// 2. Summarize the transcription with ChatSession (qwen), streaming the output.

import {
    AudioSession,
    ChatSession,
    FoundryLocalManager,
    Item,
    Request,
} from 'foundry-local-sdk';
import path from 'path';
import { fileURLToPath } from 'url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

const CHAT_MODEL = 'qwen2.5-0.5b';
const WHISPER_MODEL = 'whisper-tiny';

async function main() {
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

    const catalog = manager.catalog;

    console.log('\n--- Loading models ---');

    const chatModel = await catalog.getModel(CHAT_MODEL);
    if (!chatModel) {
        throw new Error(
            `Chat model "${CHAT_MODEL}" not found. Run "foundry model list" to see available models.`
        );
    }

    const whisperModel = await catalog.getModel(WHISPER_MODEL);
    if (!whisperModel) {
        throw new Error(
            `Whisper model "${WHISPER_MODEL}" not found. Run "foundry model list" to see available models.`
        );
    }

    if (!chatModel.isCached) {
        console.log(`Downloading ${CHAT_MODEL}...`);
        await chatModel.download((progress) => {
            process.stdout.write(`\r  ${CHAT_MODEL}: ${progress.toFixed(1)}%`);
        });
        console.log();
    }

    if (!whisperModel.isCached) {
        console.log(`Downloading ${WHISPER_MODEL}...`);
        await whisperModel.download((progress) => {
            process.stdout.write(`\r  ${WHISPER_MODEL}: ${progress.toFixed(1)}%`);
        });
        console.log();
    }

    console.log(`Loading ${CHAT_MODEL}...`);
    await chatModel.load();
    console.log(`Loading ${WHISPER_MODEL}...`);
    await whisperModel.load();
    console.log('Both models loaded.\n');

    // --- Step 1: Transcribe audio ---
    console.log('=== Step 1: Audio Transcription ===');
    const audioFilePath = path.resolve(__dirname, '..', 'Recording.mp3');

    let transcription = '';
    const audioSession = new AudioSession(whisperModel);
    try {
        audioSession.setOptions({ additionalOptions: { language: 'en' } });
        const request = new Request();
        request.addItem(Item.audioFromUri(audioFilePath));
        for await (const item of audioSession.processStreamingRequest(request)) {
            if (item.type === 'speechSegment' && item.text) {
                transcription += item.text;
            }
        }
    } finally {
        audioSession.dispose();
    }
    console.log('You said:', transcription);

    // --- Step 2: Analyze with chat model ---
    console.log('\n=== Step 2: AI Analysis ===');
    const chatSession = new ChatSession(chatModel);
    try {
        const request = new Request();
        request.addItem(Item.systemMessage(
            'You are a helpful assistant. Summarize the following transcribed audio and ' +
            'extract key themes and action items.'
        ));
        request.addItem(Item.userMessage(transcription));
        request.setOptions({ search: { temperature: 0.7, maxOutputTokens: 500 } });

        console.log('Generating summary...\n');
        for await (const item of chatSession.processStreamingRequest(request)) {
            if (item.type === 'text' && item.text) {
                process.stdout.write(item.text);
            }
        }
        console.log('\n');
    } finally {
        chatSession.dispose();
    }

    await chatModel.unload();
    await whisperModel.unload();
    console.log('Done.');
}

main().catch(console.error);
