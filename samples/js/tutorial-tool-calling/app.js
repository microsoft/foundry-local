// Tutorial: Tool Calling — Foundry Local JS SDK (native session API).
//
// Registers two tools on a ChatSession, drives a turn-loop that collects
// ToolCallItems from the stream, executes them locally, and pushes
// ToolResultItems back into the next request until the model stops calling tools.

import { ChatSession, FoundryLocalManager, Item, Request } from 'foundry-local-sdk';
import * as readline from 'readline';

// --- Tool implementations ---
function getWeather(location, unit = 'celsius') {
    return {
        location,
        temperature: unit === 'celsius' ? 22 : 72,
        unit,
        condition: 'Sunny',
    };
}

function calculate(expression) {
    // Input is validated against a strict allowlist of numeric/math characters,
    // making this safe from code injection in this tutorial context.
    const allowed = /^[0-9+\-*/(). ]+$/;
    if (!allowed.test(expression)) {
        return { error: 'Invalid expression' };
    }
    try {
        const result = Function(`"use strict"; return (${expression})`)();
        return { expression, result };
    } catch (err) {
        return { error: err.message };
    }
}

const toolFunctions = {
    get_weather: (args) => getWeather(args.location, args.unit),
    calculate: (args) => calculate(args.expression),
};

// --- Tool definitions ---
const toolDefinitions = [
    {
        name: 'get_weather',
        description: 'Get the current weather for a location',
        jsonSchema: JSON.stringify({
            type: 'object',
            properties: {
                location: { type: 'string', description: 'The city or location' },
                unit: {
                    type: 'string',
                    enum: ['celsius', 'fahrenheit'],
                    description: 'Temperature unit',
                },
            },
            required: ['location'],
        }),
    },
    {
        name: 'calculate',
        description: 'Perform a math calculation',
        jsonSchema: JSON.stringify({
            type: 'object',
            properties: {
                expression: { type: 'string', description: 'The math expression to evaluate' },
            },
            required: ['expression'],
        }),
    },
];

// --- Main application ---
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

const model = await manager.catalog.getModel('qwen2.5-0.5b');

await model.download((progress) => {
    process.stdout.write(`\rDownloading model: ${progress.toFixed(2)}%`);
});
console.log('\nModel downloaded.');

await model.load();
console.log('Model loaded and ready.');

const session = new ChatSession(model);

try {
    for (const def of toolDefinitions) {
        session.addToolDefinition(def);
    }

    const rl = readline.createInterface({ input: process.stdin, output: process.stdout });
    const askQuestion = (prompt) => new Promise((resolve) => rl.question(prompt, resolve));

    console.log("\nTool-calling assistant ready! Type 'quit' to exit.\n");

    let firstTurn = true;
    while (true) {
        const userInput = await askQuestion('You: ');
        const trimmed = userInput.trim().toLowerCase();
        if (trimmed === 'quit' || trimmed === 'exit') {
            break;
        }

        let request = new Request();
        if (firstTurn) {
            request.addItem(Item.systemMessage(
                'You are a helpful assistant with access to tools. ' +
                'Use them when needed to answer questions accurately.'
            ));
            firstTurn = false;
        }
        request.addItem(Item.userMessage(userInput));

        // Loop until the model stops emitting tool calls.
        while (true) {
            const toolCalls = [];
            let answerText = '';

            for await (const item of session.processStreamingRequest(request)) {
                if (item.type === 'toolCall') {
                    toolCalls.push(item);
                } else if (item.type === 'text' && item.text) {
                    answerText += item.text;
                }
            }

            if (toolCalls.length === 0) {
                console.log(`Assistant: ${answerText}\n`);
                break;
            }

            request = new Request();
            for (const call of toolCalls) {
                const args = JSON.parse(call.arguments);
                console.log(`  Tool call: ${call.name}(${JSON.stringify(args)})`);
                const fn = toolFunctions[call.name];
                const result = fn ? fn(args) : { error: `Unknown tool: ${call.name}` };
                request.addItem(Item.toolResult(call.callId, JSON.stringify(result)));
            }
        }
    }

    rl.close();
} finally {
    session.dispose();
}

await model.unload();
console.log('Model unloaded. Goodbye!');
