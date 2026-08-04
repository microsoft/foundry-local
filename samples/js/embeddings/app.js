// <complete_code>
// <imports>
import { EmbeddingsSession, FoundryLocalManager, Item, Request } from 'foundry-local-sdk';
// </imports>

function tensorToFloats(tensor) {
    if (tensor.dataType !== 'float') {
        throw new TypeError(`Expected a float tensor, received ${tensor.dataType}`);
    }

    if (tensor.data.byteOffset % Float32Array.BYTES_PER_ELEMENT !== 0) {
        throw new RangeError('Tensor data is not aligned for a Float32Array view');
    }

    return new Float32Array(
        tensor.data.buffer,
        tensor.data.byteOffset,
        tensor.data.byteLength / Float32Array.BYTES_PER_ELEMENT
    );
}

// Initialize the Foundry Local SDK
console.log('Initializing Foundry Local SDK...');

// <init>
const manager = FoundryLocalManager.create({
    appName: 'foundry_local_samples',
    logLevel: 'info'
});
// </init>
console.log('✓ SDK initialized successfully');

// <model_setup>
// Get an embedding model
const modelAlias = 'qwen3-embedding-0.6b';
const model = await manager.catalog.getModel(modelAlias);

// Download the model
console.log(`\nDownloading model ${modelAlias}...`);
await model.download((progress) => {
    process.stdout.write(`\rDownloading... ${progress.toFixed(2)}%`);
});
console.log('\n✓ Model downloaded');

// Load the model
console.log(`\nLoading model ${modelAlias}...`);
await model.load();
console.log('✓ Model loaded');
// </model_setup>

const session = new EmbeddingsSession(model);
try {
    // <single_embedding>
    console.log('\n--- Single Embedding ---');
    const req = new Request();
    req.addItem(Item.text('The quick brown fox jumps over the lazy dog'));
    const resp = await session.processRequest(req);

    const tensor = resp.output.find(it => it.type === 'tensor');
    console.log(`Shape: ${tensor.shape.join('x')}`);
    const floats = tensorToFloats(tensor);
    console.log(`First 5 values: [${Array.from(floats.slice(0, 5)).map(v => v.toFixed(6)).join(', ')}]`);
    // </single_embedding>

    // <batch_embedding>
    console.log('\n--- Batch Embeddings ---');
    const batchReq = new Request();
    batchReq.addItem(Item.text('Machine learning is a subset of artificial intelligence'));
    batchReq.addItem(Item.text('The capital of France is Paris'));
    batchReq.addItem(Item.text('Rust is a systems programming language'));
    const batchResp = await session.processRequest(batchReq);

    const tensors = batchResp.output.filter(it => it.type === 'tensor');
    console.log(`Number of embeddings: ${tensors.length}`);
    for (let i = 0; i < tensors.length; i++) {
        console.log(`  [${i}] Shape: ${tensors[i].shape.join('x')}`);
    }
    // </batch_embedding>
} finally {
    session.dispose();
}

// <cleanup>
// Unload the model
console.log('\nUnloading model...');
await model.unload();
console.log('✓ Model unloaded');
// </cleanup>
// </complete_code>
