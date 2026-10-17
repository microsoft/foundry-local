// <complete_code>
// <imports>
import { ChatSession, FoundryLocalManager, Item, Request } from 'foundry-local-sdk';
import * as readline from 'readline';
// </imports>

const toolSchemas = {
    get_weather: {
        type: 'object',
        properties: {
            location: {
                type: 'string',
                description: 'The city or location'
            },
            unit: {
                type: 'string',
                enum: ['celsius', 'fahrenheit'],
                description: 'Temperature unit'
            }
        },
        required: ['location']
    },
    calculate: {
        type: 'object',
        properties: {
            expression: {
                type: 'string',
                description: 'The math expression to evaluate'
            }
        },
        required: ['expression']
    }
};

function getWeather(location, unit = 'celsius') {
    return {
        location,
        temperature: unit === 'celsius' ? 22 : 72,
        unit,
        condition: 'Sunny'
    };
}

function calculate(expression) {
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
    calculate: (args) => calculate(args.expression)
};

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

async function resolveToolCalls(session, request) {
    let response = await session.processRequest(request);

    while (true) {
        const toolCalls = response.output.filter((item) => item.type === 'toolCall');
        if (toolCalls.length === 0) {
            return collectText(response.output);
        }

        const followUp = new Request().setOptions({ toolChoice: 'auto' });
        for (const toolCall of toolCalls) {
            const args = JSON.parse(toolCall.arguments || '{}');
            console.log(`  Tool call: ${toolCall.name}(${JSON.stringify(args)})`);
            const result = toolFunctions[toolCall.name](args);
            followUp.addItem(Item.toolResult(toolCall.callId, JSON.stringify(result)));
        }

        response = await session.processRequest(followUp);
    }
}

// <init>
const manager = FoundryLocalManager.create({
    appName: 'foundry_local_samples',
    logLevel: 'info'
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
session.setOptions({
    search: {
        temperature: 0.7,
        maxOutputTokens: 512
    }
});
session.addToolDefinition({
    name: 'get_weather',
    description: 'Get the current weather for a location',
    jsonSchema: JSON.stringify(toolSchemas.get_weather)
});
session.addToolDefinition({
    name: 'calculate',
    description: 'Perform a math calculation',
    jsonSchema: JSON.stringify(toolSchemas.calculate)
});
// </init>

const rl = readline.createInterface({
    input: process.stdin,
    output: process.stdout
});

const askQuestion = (prompt) =>
    new Promise((resolve) => rl.question(prompt, resolve));

console.log('\nTool-calling assistant ready! Type \'quit\' to exit.\n');

let firstTurn = true;

try {
    while (true) {
        const userInput = await askQuestion('You: ');
        if (
            userInput.trim().toLowerCase() === 'quit' ||
            userInput.trim().toLowerCase() === 'exit'
        ) {
            break;
        }

        const request = new Request();
        if (firstTurn) {
            request.addItem(Item.systemMessage(
                'You are a helpful assistant with access to tools. Use them when needed to answer questions accurately.'
            ));
            firstTurn = false;
        }
        request.addItem(Item.userMessage(userInput));

        const answer = await resolveToolCalls(session, request);
        console.log(`Assistant: ${answer}\n`);
    }
} finally {
    session.dispose();
    await model.unload();
    console.log('Model unloaded. Goodbye!');
    rl.close();
}
// </complete_code>
