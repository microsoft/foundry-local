import { describe, expect, it } from "vitest";

import { ChatSession, type ToolDefinition } from "../src/session.js";

function callAddToolDefinition(definition: ToolDefinition): void {
  ChatSession.prototype.addToolDefinition.call({} as ChatSession, definition);
}

describe("ChatSession tool definition validation", () => {
  it.each([
    [{ name: "bad\0name", description: "description", jsonSchema: "{}" }, "definition.name"],
    [{ name: "name", description: "bad\0description", jsonSchema: "{}" }, "definition.description"],
    [{ name: "name", description: "description", jsonSchema: "{\0}" }, "definition.jsonSchema"],
    [{ name: "bad\0name", description: "description", kind: "custom" }, "definition.name"],
    [{ name: "name", description: "bad\0description", kind: "custom" }, "definition.description"],
  ] as const)("rejects an embedded NUL before calling native code", (definition, argumentName) => {
    expect(() => callAddToolDefinition(definition)).toThrow(
      new TypeError(`${argumentName} must not contain an embedded NUL character`),
    );
  });

  it("rejects an embedded NUL in a removal name before calling native code", () => {
    expect(() => ChatSession.prototype.removeToolDefinition.call({} as ChatSession, "bad\0name")).toThrow(
      new TypeError("name must not contain an embedded NUL character"),
    );
  });
});
