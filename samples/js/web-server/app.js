import { FoundryLocalManager } from "foundry-local-sdk";

const modelAlias = process.argv[2] ?? "qwen2.5-0.5b";
const manager = FoundryLocalManager.create({
  appName: "foundry_local_samples",
  logLevel: "info",
  webServiceUrls: "http://127.0.0.1:0",
});

let model;
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

  manager.startWebService();
  const endpoint = manager.urls[0];
  if (endpoint === undefined) {
    throw new Error("The built-in web server did not report a bound URL.");
  }
  console.log(`Server listening at ${endpoint}`);

  const response = await fetch(`${endpoint.replace(/\/$/, "")}/v1/chat/completions`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({
      model: model.id,
      messages: [{ role: "user", content: "What is the golden ratio? Answer in one sentence." }],
      temperature: 0,
      max_tokens: 128,
    }),
  });
  if (!response.ok) {
    throw new Error(`Chat request failed (${response.status}): ${await response.text()}`);
  }

  const completion = await response.json();
  const content = completion?.choices?.[0]?.message?.content;
  if (typeof content !== "string") {
    throw new Error(`Unexpected response: ${JSON.stringify(completion)}`);
  }
  console.log(`Assistant: ${content}`);
} finally {
  try {
    if (manager.isWebServiceRunning) {
      manager.stopWebService();
    }
  } finally {
    try {
      if (model !== undefined && modelLoaded) {
        await model.unload();
      }
    } finally {
      manager.dispose();
    }
  }
}
