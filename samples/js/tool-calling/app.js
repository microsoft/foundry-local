import { ChatSession, FoundryLocalManager, Item, Request } from "foundry-local-sdk";

const manager = FoundryLocalManager.create({
  appName: "foundry_local_samples",
  logLevel: "info",
});

let model;
let session;
let modelLoaded = false;

try {
  await manager.downloadAndRegisterEps();
  model = await manager.catalog.getModel("qwen2.5-0.5b");
  if (model.supportsToolCalling !== true) {
    throw new Error("The selected qwen2.5-0.5b variant does not support tool calling.");
  }
  console.log(`Selected tool-capable model: ${model.info.displayName ?? model.alias} (${model.id})`);

  if (!model.isCached) {
    console.log(`Downloading ${model.alias}...`);
    await model.download();
  }
  await model.load();
  modelLoaded = true;

  session = new ChatSession(model);
  session.addToolDefinition({
    name: "get_weather",
    description: "Get the current weather for a city.",
    jsonSchema: JSON.stringify({
      type: "object",
      properties: {
        city: { type: "string" },
      },
      required: ["city"],
      additionalProperties: false,
    }),
  });

  const toolRequest = new Request()
    .addItem(Item.userMessage("What is the weather in Paris?"))
    .setOptions({ toolChoice: "required", search: { temperature: 0, maxOutputTokens: 256 } });
  const toolResponse = await session.processRequest(toolRequest);
  const toolCall = toolResponse.output.find((item) => item.type === "toolCall");
  if (toolCall?.type !== "toolCall" || toolCall.name !== "get_weather") {
    throw new Error("The model did not return the expected get_weather tool call.");
  }

  console.log(`Tool call: ${toolCall.name}(${toolCall.arguments})`);
  const result = JSON.stringify({ temperature: 22, unit: "celsius", condition: "sunny" });

  const finalRequest = new Request()
    .addItem(Item.toolResult(toolCall.callId, result))
    .setOptions({ toolChoice: "none", search: { temperature: 0, maxOutputTokens: 256 } });
  const finalResponse = await session.processRequest(finalRequest);
  const answer = finalResponse.output
    .map((item) => {
      if (item.type === "text") return item.text;
      if (item.type === "message") return item.content ?? "";
      return "";
    })
    .join("");
  console.log(`Assistant: ${answer}`);
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
