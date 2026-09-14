# C++ custom tool payload invariants

- `ParsedToolCall` keeps both the parsed argument value and the exact lexical source span. Do not
  reconstruct custom fallback payloads with `json::dump()`: it loses whitespace, member order,
  escapes, and duplicate keys.
- Only an exact lone `{"input": <string>}` custom wrapper is decoded. Every other valid JSON shape
  is delivered as its original argument-value bytes.
- Custom payloads are NUL-free UTF-8. Validate generated payloads before streaming and validate
  supplied/replayed payloads before transcript normalization.
- OpenAI JSON chat requests are self-contained. Take one session-tool snapshot for compatibility
  checks and build pre-serialized tools in a request-local vector; never add unnamed JSON tools to
  the session registry.
- `flToolDefinition` version 1 ends at `json_schema` and retains empty-name pre-serialized
  compatibility. Version 2 appends `kind` and rejects empty public function/custom names.
- Response-store replay keeps a private raw-arguments sidecar on `ToolCallItem`. This preserves
  arbitrary custom text until kind resolution while retaining the established empty-arguments
  projection for malformed function calls.
