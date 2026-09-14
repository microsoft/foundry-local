import type { ChatSession, ToolDefinition } from "../src/session.js";

const implicitFunctionTool = {
  name: "get_weather",
  description: "Gets the weather.",
  jsonSchema: '{"type":"object"}',
} satisfies ToolDefinition;

const explicitFunctionTool = {
  name: "get_weather",
  description: "Gets the weather.",
  jsonSchema: '{"type":"object"}',
  kind: "function",
} satisfies ToolDefinition;

const customTool = {
  name: "apply_patch",
  description: "Applies a patch.",
  kind: "custom",
} satisfies ToolDefinition;

// @ts-expect-error — a function tool requires a JSON schema
const functionWithoutSchema: ToolDefinition = {
  name: "get_weather",
  description: "Gets the weather.",
};

// @ts-expect-error — an explicitly tagged function tool also requires a JSON schema
const explicitFunctionWithoutSchema: ToolDefinition = {
  name: "get_weather",
  description: "Gets the weather.",
  kind: "function",
};

// @ts-expect-error — a custom tool must not define a JSON schema
const customWithSchema: ToolDefinition = {
  name: "apply_patch",
  description: "Applies a patch.",
  kind: "custom",
  jsonSchema: '{"type":"object"}',
};

function assertMethodSignatures(target: ChatSession): void {
  target.addToolDefinition(implicitFunctionTool);
  target.addToolDefinition(explicitFunctionTool);
  target.addToolDefinition(customTool);
  target.addCustomToolDefinition({ name: "apply_patch", description: "Applies a patch." });

  // @ts-expect-error — the custom-tool convenience API uses one object argument
  target.addCustomToolDefinition("apply_patch", "Applies a patch.");
  // @ts-expect-error — the custom-tool convenience API does not accept a schema
  target.addCustomToolDefinition({ name: "apply_patch", description: "Applies a patch.", jsonSchema: "{}" });
}

void assertMethodSignatures;
void functionWithoutSchema;
void explicitFunctionWithoutSchema;
void customWithSchema;
