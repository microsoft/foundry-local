# Tool calling

This sample uses native `ChatSession` tool definitions and the `ToolCallItem`/`ToolResultItem` flow. It forces the
first turn to call `get_weather` for Paris, supplies a mock weather result, and submits it to the same session for
the final answer with tool calling disabled.

```bash
npm install
npm start
```

The sample selects `qwen2.5-0.5b` and verifies that the selected catalog variant declares tool-calling support. This
keeps the download small and uses a model whose current variants publish the metadata required by structured tool
calling.
