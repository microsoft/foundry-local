// <complete_code>
// <imports>
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { FoundryLocalManager } from 'foundry-local-sdk';
// </imports>

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const DEFAULT_MODEL_ALIAS = 'qwen3.5-0.8b';
const DEFAULT_MAX_OUTPUT_TOKENS = 8192;
const endpointUrl = 'http://localhost:5765';

const argv = process.argv.slice(2);
if (argv.length < 1) {
    console.error('Usage: node app.js <model_alias_or_id> [image_path]');
    console.error('         node app.js --list-models');
    console.error('  Example: node app.js qwen3.5-0.8b');
    console.error('  Example: node app.js Qwen2.5-VL-7B-Instruct-generic-cpu');
    process.exit(1);
}

const listModels = argv[0] === '--list-models' || argv[0] === '-l';
const modelIdentifier = listModels ? null : argv[0];
const defaultImage = path.join(__dirname, 'test_image.jpg');
const imagePath = !listModels && argv.length > 1 ? argv[1] : defaultImage;

// <init>
console.log('Initializing Foundry Local SDK...');
const manager = FoundryLocalManager.create({
    appName: 'foundry_local_samples',
    logLevel: 'info',
    webServiceUrls: endpointUrl,
});
console.log('✓ SDK initialized successfully');

console.log('\nDownloading execution providers:');
let currentEp = '';
await manager.downloadAndRegisterEps((epName, percent) => {
    if (epName !== currentEp) {
        if (currentEp !== '') process.stdout.write('\n');
        currentEp = epName;
    }
    process.stdout.write(`\r  ${epName.padEnd(30)}  ${percent.toFixed(1).padStart(5)}%`);
});
if (currentEp !== '') process.stdout.write('\n');
// </init>

if (listModels) {
    const allModels = await manager.catalog.listModels();
    const visionModels = allModels
        .filter((m) => (m.info?.task ?? '').toLowerCase().includes('vision'))
        .sort((a, b) => a.alias.localeCompare(b.alias));

    if (visionModels.length === 0) {
        console.log('\nNo vision models found in catalog.');
        process.exit(0);
    }

    const totalVariants = visionModels.reduce((sum, m) => sum + (m.variants?.length ?? 0), 0);
    console.log(`\nVision models in catalog (${visionModels.length} aliases, ${totalVariants} variants):`);
    console.log(`  ${'ALIAS'.padEnd(32)}  ${'INPUT MODALITIES'.padEnd(20)}  ${'OUTPUT MODALITIES'.padEnd(20)}  ${'TASK'.padEnd(24)}  CAPABILITIES`);
    for (const m of visionModels) {
        const task = m.info?.task ?? '';
        const capabilities = m.info?.capabilities ?? '';
        const inMod = m.info?.inputModalities ?? '';
        const outMod = m.info?.outputModalities ?? '';
        console.log(`  ${m.alias.padEnd(32)}  ${inMod.padEnd(20)}  ${outMod.padEnd(20)}  ${task.padEnd(24)}  ${capabilities}`);

        const variants = [...(m.variants ?? [])].sort((a, b) => {
            const ad = a.info?.runtime?.deviceType ?? '';
            const bd = b.info?.runtime?.deviceType ?? '';
            if (ad !== bd) return ad.localeCompare(bd);
            const ae = a.info?.runtime?.executionProvider ?? '';
            const be = b.info?.runtime?.executionProvider ?? '';
            if (ae !== be) return ae.localeCompare(be);
            return a.id.localeCompare(b.id);
        });
        if (variants.length === 0) continue;

        console.log(`      ${'VARIANT ID'.padEnd(54)}  ${'DEVICE'.padEnd(6)}  ${'EXECUTION PROVIDER'.padEnd(32)}  ${'SIZE (MB)'.padStart(10)}  CACHED`);
        for (const v of variants) {
            const device = v.info?.runtime?.deviceType ?? '';
            const ep = v.info?.runtime?.executionProvider ?? '';
            const size = v.info?.fileSizeMb != null ? String(v.info.fileSizeMb).padStart(10) : ''.padStart(10);
            const cached = (await v.isCached()) ? 'yes' : 'no';
            console.log(`      ${v.id.padEnd(54)}  ${device.padEnd(6)}  ${ep.padEnd(32)}  ${size}  ${cached}`);
        }
    }
    process.exit(0);
}

// <model_setup>
let model = await manager.catalog.getModel(modelIdentifier);
if (!model) {
    model = await manager.catalog.getModelVariant(modelIdentifier);
}
if (!model) {
    const allModels = await manager.catalog.listModels();
    console.error(`\nModel '${modelIdentifier}' not found in catalog (tried alias and variant id).`);
    console.error(`Available aliases: ${allModels.map((m) => m.alias).join(', ')}`);
    console.error('Run with --list-models to see variant ids.');
    process.exit(1);
}

if (!model.isCached) {
    console.log(`\nDownloading model ${modelIdentifier}...`);
    await model.download((progress) => {
        process.stdout.write(`\rDownloading model: ${progress.toFixed(2)}%`);
    });
    console.log('\nModel downloaded');
}

console.log('\nLoading model...');
await model.load();
console.log('Model loaded');
// </model_setup>

// <server_setup>
console.log('\nStarting web service...');
manager.startWebService();
const baseUrl = endpointUrl.replace(/\/+$/, '') + '/v1';
console.log(`Web service started on ${baseUrl}`);
// </server_setup>

// <inference>
console.log(`\nPreparing image: ${imagePath}`);
const { base64: imageB64, mediaType } = encodeImage(imagePath);

// The Foundry Local Responses API accepts an array of message items with input_text /
// input_image content parts. The input_image part uses Foundry-specific `image_data` and
// `media_type` fields (in place of OpenAI's `image_url`).
const visionInput = [
    {
        type: 'message',
        role: 'user',
        content: [
            { type: 'input_text', text: 'Describe this image.' },
            { type: 'input_image', image_data: imageB64, media_type: mediaType },
        ],
    },
];

console.log('\nStreaming vision response...');
const response = await fetch(`${baseUrl}/responses`, {
    method: 'POST',
    headers: {
        'Content-Type': 'application/json',
        Accept: 'text/event-stream',
        Authorization: 'Bearer notneeded',
    },
    body: JSON.stringify({
        model: model.id,
        input: visionInput,
        max_output_tokens: DEFAULT_MAX_OUTPUT_TOKENS,
        stream: true,
    }),
});
if (!response.ok) {
    throw new Error(`Responses API error: ${response.status} ${await response.text()}`);
}

process.stdout.write('[ASSISTANT]: ');
const decoder = new TextDecoder();
let buf = '';
for await (const chunk of response.body) {
    buf += decoder.decode(chunk, { stream: true });
    let nl;
    while ((nl = buf.indexOf('\n')) !== -1) {
        const line = buf.slice(0, nl).trimEnd();
        buf = buf.slice(nl + 1);
        if (!line.startsWith('data: ')) continue;
        const data = line.slice('data: '.length);
        if (data === '[DONE]') break;
        try {
            const event = JSON.parse(data);
            if (event.type === 'response.output_text.delta' && typeof event.delta === 'string') {
                process.stdout.write(event.delta);
            }
        } catch {
            // ignore keepalives or non-JSON lines
        }
    }
}
process.stdout.write('\n');
// </inference>

console.log('\nUnloading model and stopping web service...');
await model.unload();
manager.stopWebService();
console.log('✓ Model unloaded and web service stopped');

function encodeImage(p) {
    const mediaTypes = {
        '.jpg': 'image/jpeg',
        '.jpeg': 'image/jpeg',
        '.png': 'image/png',
        '.gif': 'image/gif',
        '.bmp': 'image/bmp',
        '.webp': 'image/webp',
    };
    const ext = path.extname(p).toLowerCase();
    const mediaType = mediaTypes[ext] ?? 'image/jpeg';
    const bytes = fs.readFileSync(p);
    return { base64: bytes.toString('base64'), mediaType };
}
// </complete_code>
