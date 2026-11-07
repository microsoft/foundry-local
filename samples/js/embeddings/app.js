import { EmbeddingsSession, FoundryLocalManager, Item, Request } from "foundry-local-sdk";

const modelAlias = process.argv[2] ?? "qwen3-embedding-0.6b";
const inputs = [
  "A kitten is sleeping on a rug.",
  "A cat rests on a mat.",
  "Saturn has a prominent ring system.",
];

const manager = FoundryLocalManager.create({
  appName: "foundry_local_samples",
  logLevel: "info",
});

let model;
let session;
let modelLoaded = false;

try {
  await manager.downloadAndRegisterEps();
  model = await manager.catalog.getModel(modelAlias);
  if (!model.isCached) {
    console.log(`Downloading ${model.alias}...`);
    await model.download();
  }
  await model.load();
  modelLoaded = true;

  session = new EmbeddingsSession(model);
  const request = new Request();
  for (const input of inputs) {
    request.addItem(Item.text(input));
  }

  const response = await session.processRequest(request);
  const embeddings = response.output.map((item, index) => {
    if (item.type !== "tensor" || item.dataType !== "float") {
      throw new Error(`Expected float tensor output at index ${index}.`);
    }
    const bytes = item.data.slice();
    return new Float32Array(bytes.buffer, bytes.byteOffset, bytes.byteLength / Float32Array.BYTES_PER_ELEMENT);
  });

  // EmbeddingsSession returns L2-normalized vectors, so their dot product is cosine similarity.
  const cosineSimilarity = (left, right) => left.reduce((sum, value, index) => sum + value * right[index], 0);
  console.log(`Model: ${model.id}`);
  console.log(`Dimensions: ${embeddings[0].length}`);
  console.log(`Similarity (cat, cat): ${cosineSimilarity(embeddings[0], embeddings[1])}`);
  console.log(`Similarity (cat, Saturn): ${cosineSimilarity(embeddings[0], embeddings[2])}`);
} finally {
  session?.dispose();
  try {
    if (model !== undefined && modelLoaded) {
      await model.unload();
    }
  } finally {
    manager.dispose();
  }
}
