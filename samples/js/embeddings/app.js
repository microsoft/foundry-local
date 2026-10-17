// <complete_code>
// <imports>
import { EmbeddingsSession, FoundryLocalManager, Item, Request } from 'foundry-local-sdk';
// </imports>

function tensorToFloats(item) {
    if (item.type !== 'tensor') {
        throw new TypeError(`Expected tensor output, got ${item.type}`);
    }

    if (item.dataType !== 'float') {
        throw new TypeError(`Expected float tensor output, got ${item.dataType}`);
    }

    const bytes = new Uint8Array(item.data);
    const aligned = new Uint8Array(bytes.byteLength);
    aligned.set(bytes);
    return new Float32Array(aligned.buffer);
}

console.log('Initializing Foundry Local SDK...');

// <init>
const manager = FoundryLocalManager.create({
    appName: 'foundry_local_samples',
    logLevel: 'info'
});
// </init>
console.log('✓ SDK initialized successfully');

// <model_setup>
const modelAlias = 'qwen3-embedding-0.6b';
const model = await manager.catalog.getModel(modelAlias);

console.log(`\nDownloading model ${modelAlias}...`);
await model.download((progress) => {
    process.stdout.write(`\rDownloading... ${progress.toFixed(2)}%`);
});
console.log('\n✓ Model downloaded');

console.log(`\nLoading model ${modelAlias}...`);
await model.load();
console.log('✓ Model loaded');
// </model_setup>

const session = new EmbeddingsSession(model);

try {
    // <single_embedding>
    console.log('\nCreating embeddings session...');
    console.log('✓ Embeddings session created');

    console.log('\n--- Single Embedding ---');
    const response = await session.processRequest(
        new Request().addItem(Item.text('The quick brown fox jumps over the lazy dog'))
    );

    const embedding = tensorToFloats(response.output[0]);
    console.log(`Dimensions: ${embedding.length}`);
    console.log(`First 5 values: [${Array.from(embedding.slice(0, 5)).map((v) => v.toFixed(6)).join(', ')}]`);
    // </single_embedding>

    // <batch_embedding>
    console.log('\n--- Batch Embeddings ---');
    const batchResponse = await session.processRequest(
        new Request()
            .addItem(Item.text('Machine learning is a subset of artificial intelligence'))
            .addItem(Item.text('The capital of France is Paris'))
            .addItem(Item.text('Rust is a systems programming language'))
    );

    console.log(`Number of embeddings: ${batchResponse.output.length}`);
    for (let i = 0; i < batchResponse.output.length; i++) {
        console.log(`  [${i}] Dimensions: ${tensorToFloats(batchResponse.output[i]).length}`);
    }
    // </batch_embedding>
} finally {
    session.dispose();

    // <cleanup>
    console.log('\nUnloading model...');
    await model.unload();
    console.log('✓ Model unloaded');
    // </cleanup>
}
// </complete_code>
