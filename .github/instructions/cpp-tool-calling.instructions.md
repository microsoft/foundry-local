---
description: "Use when changing tool schemas, structured generation, tool-call parsing, recovery, or coding-agent qualification."
applyTo: "sdk_v2/cpp/src/inferencing/generative/{chat,toolcalling}/**"
---
# Structured tool calling

- Engine and Generator use the same immutable `ToolCallPlan`. Its jsoncons validators enforce the original parameter
  schemas before publishing any call in a batch. Never coerce arguments, guess XML values, inject schema defaults,
  or silently drop constraints. Missing parameter schemas denote parameterless functions, not malformed schemas.
- Generation guidance constrains canonical JSON syntax, envelopes, names, and call cardinality, not JSON Schema
  semantics. Do not reintroduce the native JSON Schema compiler: its property-order and Unicode-escape restrictions
  can corrupt otherwise valid tool values. Keep one original-schema authority rather than handwritten subsets.
  Permit JSON whitespace at structural boundaries explicitly; never use global whitespace ignoring inside strings.
- Reasoning and executable tool boundaries use published token IDs. Added tokens may have `special:false`; quoted
  marker spellings are data. Preserve payload ownership once a canonical or explicit raw envelope has opened.
- Automatic structured output must explicitly choose `MESSAGE\n` plus ordinary text or an executable tool boundary.
  Strip text framing incrementally and project it into assistant history. Use whole-turn guidance for automatic
  turns, including nonreasoning Engine models; region-only masks cannot enforce this initial mode selection.
- Encode transcript/schema/argument data with the independent literal tokenizer. Only template-owned structure,
  runtime-owned protocol boundaries, and actual historical call boundaries use ordinary encoding. Encoding
  protocol boundary exemplars literally teaches the model to echo ordinary text instead of invoking tools.
  Do not toggle tokenizer options on shared instances or let templates inspect unprotected data.
- Recovery replaces only an uncommitted suffix, preserving already streamed tokens exactly. Permit at most one retry,
  include a bounded literal diagnostic, and share the remaining output budget. Never retry published calls or
  canceled/truncated turns. Explicit raw framing is a separate protocol, not an XML fallback.
  Recovery requires a shared schema plan and no stop sequences; always use the prepared-prompt generation path.
- Engine capacity timeouts measure stalled execution, not queue duration while resident turns make progress.
  Process the complete event batch before expiry decisions and preserve cancellation of queued requests.
- Tiny Engine tests prove native grammar/control behavior, not trained-model argument fidelity. Qualification also
  requires `test/manual/tool_call_gate.py` and the real Copilot CLI gate in `native`, `minlength`, and `oneof` profiles,
  including exact adversarial source bytes and recovery from real tool/test failures. See `sdk_v2/cpp/README.md`.
