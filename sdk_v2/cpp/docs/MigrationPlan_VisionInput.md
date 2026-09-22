# Migration Plan: Vision (Image) Input for ChatSession + Responses API

> Migrates `neutron.main` commit `611cb028d4f5f83638e994105bd161741db159b4`
> ("Add Vision Support to FoundryLocalCore via Response API", 2026-03-13)
> into the C++ SDK. This commit was overlooked during the prior sync passes
> ([MigrationPlan.md](MigrationPlan.md), [MigrationPlan_20260410.md](MigrationPlan_20260410.md),
> [MigrationPlan_20260505.md](MigrationPlan_20260505.md)).

## Scope

End-to-end image input support for vision-language (VLM) models such as
`phi3v`, `phi4mm`, `qwen2_5_vl`, `qwen3_vl`, and `fara`:

1. Public C / C++ `ChatSession` API can carry image content alongside text in
   a user message.
2. `POST /v1/responses` accepts `input_image` content parts (URL, data URL, or
   `file_id`) and routes them through the vision generation path.
3. `OnnxChatGenerator` runs the multimodal model when images are present,
   wiring `OgaMultiModalProcessor::ProcessImages` into `OgaGenerator::SetInputs`.

Out of scope (track separately):
- `POST /v1/chat/completions` `image_url` content parts (mirror once the
  Responses path is verified).
- Image *output* (only input).

## Summary of Upstream Changes (C# PR 14989371)

| File | Change |
|------|--------|
| `Service/Providers/Onnx/OnnxChatGenerator.cs` | Vision branch: `TransformMessagesForVision` rewrites `image_url` → `{type:"image"}` for the chat template, `ExtractImageBytes` decodes the base64 from a data URL, `ProcessImages` returns `NamedTensors`, generator uses `SetInputs` instead of `AppendTokenSequences`. |
| `Service/Providers/Onnx/GenAIConfig.cs` | `IsMultiModal()` adds `"qwen2_5_vl"`. |
| `Service/Providers/Onnx/OnnxLoadedModel.cs` | Renames `Processor` → `VisionProcessor`. |
| `FoundryLocalCore/Core/Responses/Contracts/ContentParts.cs` | `InputImageContent.MediaType` becomes **required** (no default). |
| `FoundryLocalCore/Core/Responses/ResponseInputConverter.cs` | Removes the "image input not supported" `NotSupportedException`. Converts local file paths to `data:` URLs and forwards them as `MessageContent.ImageUrlContent`. |
| `FoundryLocalCore/Core/AzureModelCatalog.cs`, `AggregatedModelCatalog.cs`, `ModelManager.cs` | Improved BYO model path lookup (cache scan, `model:0` ↔ `model` matching). Independent of vision; assess separately. |
| `Service/WebApplicationFactory.cs` | Enables `UseDeveloperExceptionPage` in dev. Cosmetic. |
| `test/.../OnnxChatGeneratorVisionTests.cs` | Unit tests for `TransformMessagesForVision` and `ExtractImageBytes`. |

## Supported Model Scope (matches C# upstream)

The set of vision-language model types we enable on the C++ side 
is defined by the models behind the `IsMultiModal()` gate:

- `phi3v`, `phi4mm`, `fara`, `qwen2_5_vl`, `qwen3_vl`, `qwen3_5`, `gemma4`, `mistral3`
  (`whisper` is also `IsMultiModal` but it's the audio path, not vision.)

These are the model types our model catalog currently ships. Other ORT GenAI
VLM/MMM types can be added when they enter the catalog.

ORT GenAI native checks performed only to validate API choice (not to expand
scope):

- `Generator::SetInputs` (`D:\src\github\ort.genai\src\generators.cpp:479-507`)
  extracts `input_ids` from the `NamedTensors` and calls `AppendTokens`
  internally. Do *not* also call `AppendTokenSequences` on the vision path —
  that would double-feed the prompt. Upstream C# correctly calls `SetInputs`
  only.
- `SetInputs` throws only for `IsLLM`/`IsPipe` (`Pipe` = `decoder-pipeline`).
  All in-scope types are `IsVLM`/`IsMMM`, so `SetInputs` is valid for each.
- `OgaNamedTensors` lifetime: `Generator` retains `shared_ptr<Tensor>` copies
  internally, but we still keep the wrapper as a `unique_ptr` member on
  `OnnxChatGenerator` to mirror upstream.

## Current State in C++ SDK

| Concern | Status |
|---------|--------|
| `GenAIConfig::OnnxModel::IsMultiModal()` | Recognizes `phi3v`, `whisper`, `phi4mm`, `fara`, `qwen3_vl`, `qwen3_5`. **Missing `qwen2_5_vl`** (the only addition required to match C# upstream). |
| `GenAIModelInstance::GetProcessor()` | Returns `OgaMultiModalProcessor*` for multimodal models (audio path uses it today). |
| `OnnxChatGenerator::Create` | Text-only. No vision branch, no `ProcessImages`/`SetInputs`. |
| `MessageItem` | Single `std::string content`. No multi-content (text + image) support. |
| `ImageItem` | Exists with bytes/uri/format/deleter. Not wired into chat or Responses paths. |
| `responses::InputImageContent` | Parsed (`detail`, `image_url`, `file_id`) but **discarded silently** by `AddTypedInputItems` in `response_converter.cc` — only `InputTextContent` is forwarded. No `MediaType` field. |
| `responses_handler.cc` | No special handling for image content. Whatever the converter produces flows through. |
| Public C ABI (`flMessageData`, `flImageData`, `FOUNDRY_LOCAL_ITEM_IMAGE`) | Image item type and data struct already defined; never validated end-to-end with a chat request. |

## Design Decisions

### D1. How does an image attach to a message?

**Chosen** — `MessageItem.content` becomes a typed array of parts:
`std::vector<std::shared_ptr<Item>> content`, where each part is a `TextItem`,
`ImageItem`, or `AudioItem`. This mirrors the OpenAI Chat Completions /
Responses wire format, gives the message ownership of its parts, and removes
the positional "image follows its message" convention from the queue layer.

> Note: the original draft of this plan called for `std::unique_ptr<Item>`. The
> implementation refined this to `std::shared_ptr<Item>` so `MessageItem`
> remains copyable — `ChatSession` (and other consumers) copy messages out of
> the request item queue into a working vector, and parts are immutable once
> constructed, so shared ownership has no semantic cost. Image bytes are not
> duplicated when a message is copied.

We are pre-release, so the `flMessageData` struct is revised in the same
change:

```c
typedef struct flMessageData {
  uint32_t version;
  flMessageRole role;
  const flItem* const* content_items;   // TextItem | ImageItem | AudioItem
  size_t content_items_count;
  const char* name;                     // optional
} flMessageData;
```

The legacy `const char* content` field is removed from the C ABI. Single-text
convenience belongs in the language bindings, not in the C surface:

- C++ wrapper: add `static Item Message(flMessageRole role, const std::string& text, ...)` overload (already present) that internally constructs the message with a single `TextItem` part — keep the existing string-based signature; reroute its implementation through the new array-based form.
- C# / Python / JS bindings: provide the same single-string convenience.

*Rejected alternatives:*
- Sibling `ImageItem` in the request queue with positional attachment to the
  preceding user `MessageItem`. Implicit, hard to validate, breaks down across
  multi-turn history.
- Add `std::vector<ImageItem>` directly to `MessageItem`. Single-purpose; we
  would do the same dance for audio (Phi4MM, MMM models) shortly after.
- Wrap parts in a `ContentItem` aggregate. Adds a new `flItemType` and JSON
  shape with no behavior beyond what `std::vector<unique_ptr<Item>>` already
  provides.
- Keep `flMessageData.content` as a string fast path alongside `content_items`.
  Two overlapping fields require precedence rules and double validation; the
  bindings can provide that nicety with one-line sugar.
- Reuse `flItemQueue` as the container. It carries a mutex and CV that aren't
  needed for static message content, and its push/pop ownership model is
  awkward for a fixed list.

### D2. Where does base64 decoding happen?

In the **Responses converter** (HTTP boundary), not in the chat generator.
The converter receives an `InputImageContent` with either:
- `image_url` containing a data URL (`data:image/png;base64,...`),
- `image_url` containing an `http(s)://` URL (rejected for now — match upstream
  behaviour which only honoured data URLs and local file paths), or
- `image_url` containing a `file://` or absolute local path (read from disk,
  base64-encode internally — matches upstream `ConvertLocalImageToDataUrl`).

It decodes to raw bytes and constructs an `ImageItem` with `data`+`data_size`+
`format` (derived from the data URL media type). The C++ chat generator then
just consumes raw bytes — no string parsing.

### D3. `MediaType` field

Mirror upstream: make `InputImageContent.media_type` **required** when
`image_url` is a base64 data URL whose media type cannot be inferred. Actually,
data URLs always carry their media type, so we keep `media_type` *optional*
in the contract and prefer the value from the data URL itself. Reject the
request if neither source provides one.

### D4. Multiple images per request

Pass every `ImageItem` from the final user message to ORT GenAI in message
order. The prompt contains one image marker per item and `OgaImages::Load`
receives the matching image-buffer array.

## Migration Work

### 1. `IsMultiModal` — add `qwen2_5_vl`

| | |
|---|---|
| **File** | `src/inferencing/generative/genai_config.cc` (+ `genai_config.h` comment) |
| **Change** | Append `|| type == "qwen2_5_vl"` to the disjunction. Mirrors upstream C# PR 14989371. |
| **Test** | Extend `GenAIConfigTest` to assert `qwen2_5_vl` is recognized. |
| **Effort** | Trivial (1 line + comment + test). |

### 2. Revise `MessageItem` and `flMessageData` for typed content parts

| | |
|---|---|
| **Files** | `include/foundry_local/foundry_local_c.h`, `include/foundry_local/foundry_local_cpp.h`, `src/items/message_item.h`, `src/items/item.cc`, `src/items/item.h` (factory), `src/api/items.cc` (C entrypoints), `src/contracts/chat_json.cc` and `src/contracts/responses_json.cc` (serialization sites that touch messages) |
| **Changes** | |

1. **C ABI** — in `flMessageData`, replace `const char* content` with
   `const flItem* const* content_items; size_t content_items_count;`. Bump the
   handler `Get`/`Set` for `FOUNDRY_LOCAL_ITEM_MESSAGE` to copy the part array
   in/out (each part being a `flItem*` of type `TEXT`/`IMAGE`/`AUDIO`).
   Validate part types in `MessageItem::SetMessageData`; reject anything else
   with `FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT`.
2. **C++ struct** — in `MessageItem`, replace `std::string content` with
   `std::vector<std::unique_ptr<Item>> content`. Provide a `Validate()`
   helper that checks each part's `type`. Update the existing copy ctor
   (deep-copy text parts, share/clone image bytes per part rules) or delete
   copy and rely on move — this matches `ImageItem` / `AudioItem` which are
   already non-copyable.
3. **C++ wrapper convenience** — in `foundry_local_cpp.h`:
   - Keep the existing `static Item Item::Message(flMessageRole role, const std::string& content, std::optional<std::string> name = std::nullopt)` signature. Reroute its implementation to construct the underlying message with a single-element `content_items` array containing one `TextItem` — callers don't see the change.
   - Add a new overload: `static Item Item::Message(flMessageRole role, std::vector<Item> parts, std::optional<std::string> name = std::nullopt)`. Each `Item` is moved into the message's content list. (Works against the unified `Item` class introduced when the wrapper layer collapsed `BasicItem<T>` / `ConstItem` into one type backed by `detail::Base<flItem>`.)
   - Update `Item::GetMessage()` and the `MessageContent` struct to expose
     parts instead of a single `content` string. Add a helper
     `std::string Item::GetMessageText() const` that concatenates `TextItem`
     parts for callers that don't care about non-text content. Both
     accessors work in const and mutable mode by virtue of the unified
     `Item` (`native_handle()` already returns `const flItem*`).
4. Audit every reader of `MessageItem::content` (was `std::string`) and
   migrate to either iterating parts or calling the text-concatenation
   helper. Likely sites: chat session, prompt builders, tokenizer adapters,
   JSON serializers.

| **Test** | Round-trip through C ABI: build a message with `[TextItem, ImageItem]` parts, marshal to `flMessageData`, marshal back, assert structural equality. Reject `[ToolCallItem]` part with `INVALID_ARGUMENT`. |
| **Effort** | Medium-Large (~250 lines incl. call-site migration; this is the bulk of the schema work). |

### 3. Plumb image content through `ResponseConverter::ToSessionRequest`

| | |
|---|---|
| **Files** | `src/inferencing/generative/openresponses/response_converter.cc`, `src/contracts/responses.h`, `src/contracts/responses_json.cc` |
| **Changes** | |

1. Add `std::optional<std::string> media_type` to `InputImageContent` (and JSON
   round-trip) so the field is preserved when present.
2. Rewrite `AddTypedInputItems` (in `response_converter.cc`):
   - For each `InputMessage`, build `std::vector<std::unique_ptr<Item>> parts`:
     - `InputTextContent` → `TextItem(text)`
     - `InputImageContent` → `ImageItem(...)` via `MakeImageItemFromInputImage` (below)
   - Construct one `MessageItem(role, std::move(parts), name)` per
     `InputMessage` and push it onto the request.
   - Helper `MakeImageItemFromInputImage(const InputImageContent&)`:
     - If `image_url` starts with `data:`, parse `data:<media-type>;base64,<payload>`,
       base64-decode, return `ImageItem(bytes, size, format=mime_subtype)`.
       Reject malformed forms with `FL_THROW(FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT, ...)`.
     - If `image_url` is an absolute local path or `file://` URI, read file
       contents into a buffer, infer format from extension, return
       `ImageItem(bytes, size, format)`. Throw if file is missing or unreadable.
     - Otherwise (non-data `http(s)` URL or `file_id`), throw
       `FL_THROW(FOUNDRY_LOCAL_ERROR_NOT_IMPLEMENTED, "only data URLs and local files are supported for image_url")` — matches upstream.
3. Update `responses_json.cc::from_json(InputImageContent&)` to parse
   `media_type`.
4. Add a unit test for the converter that verifies a data-URL image becomes
   a `MessageItem` whose `content` parts are `[TextItem(...), ImageItem(...)]`
   with correct format and byte count.

| **Effort** | Medium (~150 lines incl. tests). |

### 4. Vision branch in `OnnxChatGenerator`

| | |
|---|---|
| **Files** | `src/inferencing/generative/chat/onnx_chat_generator.h/.cc` |
| **Changes** | |

1. Extend the `Create` factory signature to take an optional
   `std::vector<const ImageItem*> images` parameter (default empty). The
   `ChatSession` caller will populate it from the request.
2. New private helpers (mirror upstream exactly, exposed via header for unit
   tests as in upstream):
   - `static std::string TransformMessagesForVision(const std::vector<MessageItem>& messages)` — emits a JSON messages array where the
     last user message's content becomes the structured form
     `[{"type":"image"},{"type":"text","text":…}]` so that
     `ApplyChatTemplate` inserts the model's vision sentinel tokens.
     Matches upstream C# verbatim; the in-scope model templates (Phi3V,
     Phi4MM, Fara, Qwen2.5-VL / Qwen3-VL / Qwen3.5) all handle this form.
   - `static std::vector<uint8_t> ExtractImageBytes(const ImageItem&)` — returns
     a copy of the bytes (or reads from `uri` if bytes are absent). Throws on
     malformed input.
3. Branch in `Create`:
   - If `model.IsMultiModal()` and `!images.empty()`:
     - Preserve all images in message order and emit one image marker for each.
     - Build vision-rewritten messages JSON and call
       `tokenizer.ApplyChatTemplate(..., add_generation_prompt=true)` to get
       the prompt string.
     - Build a `vector<const void*>` and `vector<size_t>`, then call
       `OgaImages::Load(buffers.data(), sizes.data(), count)` (the C++ wrapper
       at `ort_genai.h:625`).
     - `auto named_tensors = processor->ProcessImages(prompt.c_str(), images.get());`
     - Create `OgaGenerator` from `gen_params`, then
       `generator->SetInputs(*named_tensors)`. **Do not** call
       `AppendTokenSequences` — `Generator::SetInputs`
       (`generators.cpp:479-507`) extracts `input_ids` from the named tensors
       and calls `AppendTokens` internally.
     - Read `input_token_count = generator->TokenCount()` after `SetInputs`
       for telemetry / `max_length` budgeting (matches the example at
       `model_mm.cpp:147`). Avoids a redundant tokenizer encode.
   - Else: existing text-only path.
4. Store `std::unique_ptr<OgaNamedTensors> named_tensors_` on the generator
   so its lifetime matches the generator. Note: `Generator` already keeps
   `shared_ptr<Tensor>` copies internally, so this is defensive belt-and-
   suspenders that also matches upstream C#.
5. Disable continuous-decoding (`AppendMessages`) for vision — first turn must
   create a fresh generator. Either return early if `cached_generator_` exists
   when an image is present, or destroy the cache and re-create. Document the
   choice in `chat_session.cc` (this is an instance of the upstream "vision
   path always rebuilds" implicit behaviour).

| **Effort** | Large (~250 lines incl. unit tests and lifetime plumbing). |

### 5. `ChatSession` — collect images from message parts

| | |
|---|---|
| **File** | `src/inferencing/generative/chat/chat_session.cc` |
| **Changes** | |

1. In `ProcessRequestImpl` and the cached-generator turn loop, build
   `std::vector<MessageItem> new_messages` as today. For the most recent
   user message, also collect
   `std::vector<const ImageItem*> images_for_last_user_message` by
   iterating its `content` parts.
2. Pass the image vector to `OnnxChatGenerator::Create`.
3. If images are present, force the non-cached path (see #4 above).
4. Vision turn does **not** participate in `TurnRecord` rollback for now —
   undoing a vision turn requires re-running `SetInputs`, which the cached
   path does not support. Document the limitation; reject `UndoTurns()` on a
   session whose history contains a vision turn (or rebuild fresh on next
   turn). Choose during implementation; default to "rebuild fresh".
5. When committing to history, collapse multi-part user content to its text
   parts only — the raw image bytes are not retained across turns (matches
   upstream: the image is only used to derive `NamedTensors` for the current
   turn).

| **Effort** | Small-Medium (~80 lines). |

### 6. Tests

| | |
|---|---|
| **Files** | `test/inferencing/generative/chat/onnx_chat_generator_vision_test.cc` (new), `test/inferencing/generative/openresponses/response_converter_test.cc` (extend), `test/integration/responses_vision_test.cc` (new, integration) |

1. **Unit (no model load)** — port the upstream four `TransformMessagesForVision`
   cases and the three `ExtractImageBytes` cases:
   - text-only message preserved
   - image content rewritten to `{type:"image"}`
   - unknown content types preserved
   - multiple messages, all transformed
   - valid base64 data URL → bytes
   - malformed base64 → throws `FOUNDRY_LOCAL_ERROR_INVALID_ARGUMENT`
   - missing `base64,` marker → throws
2. **Unit (converter)** — InputImageContent with data URL becomes an
   `ImageItem` adjacent to the `MessageItem`, with correct `format` and byte
   count. Local file path is read from disk.
3. **Integration** — gated on a vision-capable model fixture (`phi3v` or
   `qwen2_5_vl`). One round trip via `POST /v1/responses` with a tiny PNG
   data URL; assert non-empty assistant text and no error. Use
   `--gtest_filter` per the C++ testing instructions to keep it fast.

| **Effort** | Medium. |

### 7. Documentation

| | |
|---|---|
| **Files** | `docs/CppPortGuide.md` (vision section), `examples/responses/` (add a vision example), `README.md` (note vision support) |
| **Effort** | Small. |

## Out of Scope / Follow-ups

- **`POST /v1/chat/completions` `image_url` content parts.** Same plumbing as
  the Responses path, but through `chat_completions_converter.cc`. Add as a
  follow-up once Responses is verified.
- **`http(s)://` image URLs.** Upstream did not download remote URLs; we
  mirror that. Add later behind a feature flag if needed.
- **Multiple images per request.** Reject with a clear error initially.
- **BYO model path lookup improvements** from the same upstream PR
  (`AzureModelCatalog`/`AggregatedModelCatalog`/`ModelManager`). These are
  unrelated to vision and should be tracked separately.

### Open Responses spec elements deliberately kept in the web layer

The Open Responses spec defines several requirements that are wire-protocol
concerns, not direct-API concerns. They stay in
`src/inferencing/generative/openresponses/` and do **not** require additions
to `Item` / `Session` / `Request` / `Response`:

- **Per-item `id`.** Already minted in the web layer by
  `ResponseConverter::GenerateId(prefix)` for `resp_*`, `msg_*`, `fc_*`,
  `call_*`. Direct-API callers hold the `Item*` and don't need an opaque
  string handle. If a wire id needs to round-trip with an item (e.g., on a
  continuation turn), use the existing `Item::GetMetadata()` key-value bag
  (`metadata["id"] = "msg_..."`) — no ABI change.
- **Per-item `status` (`in_progress` / `completed` / `incomplete` / `failed`).**
  A streaming-protocol concept. Direct-API callers observe lifecycle through
  the queue / future they're already waiting on. Status is emitted by the SSE
  serializer based on the response/turn's own state, not stored on the items
  themselves.
- **`previous_response_id` resumption.** Already covered by
  `SessionManager::CheckOut(key)` / `CheckIn(key, session)` (KV-cache reuse)
  plus `ResponseStore` (full input/output rehydration). The web layer maps
  `previous_response_id` to the cache key.
- **Streaming events** (`response.output_item.added`,
  `response.content_part.delta`, etc.). Generated by the SSE writer from
  the existing `ItemQueue` output stream; the wire event model does not
  reach into core item shapes.
- **`AssistantContent` / `ModelContent` union.** The spec keeps user content
  multimodal and model content narrow; assistant-only outputs (reasoning,
  tool calls, hosted-tool receipts) are emitted as **distinct output
  items** in the Response, not as a different content shape on a message.
  Our existing `TextItem` / `ToolCallItem` / `ToolResultItem` already match
  this pattern. A future `ReasoningItem` would slot in as a new item type
  when needed; no `MessageItem` change is required to support it.
- **Refusal content / output text annotations** (`url_citation`,
  `file_citation`). OpenAI extensions with no on-device producer.

## Order of Implementation

1. (#1) `qwen2_5_vl` — trivial, can land first.
2. (#2) `MessageItem` / `flMessageData` schema revision + call-site
   migration. Lands as its own change so the rest of the work builds on the
   new shape; touches the most files.
3. (#4) `OnnxChatGenerator` vision branch + unit tests.
4. (#5) `ChatSession` request scanning + cached-generator handling.
5. (#3) Responses converter wiring.
6. (#6) Integration test against a real VLM model fixture.
7. (#7) Docs and example.

## Risk

| Risk | Mitigation |
|------|------------|
| `OgaImages::Load`/`LoadImagesFromBuffers` API surface differs across ORT GenAI versions. | Confirmed present in current ORT GenAI source (`src/ort_genai.h:625`) and in our bundled headers. |
| Cached-generator + vision interaction. | Force fresh generator when images present; document in `chat_session.h`. |
| `OgaGenerator::SetInputs` lifetime. | Generator internally retains `shared_ptr<Tensor>`, so memory safety is guaranteed; we still keep the `OgaNamedTensors` wrapper alive on the generator for symmetry with upstream. |
| Chat template differences across in-scope models break the structured `{type:"image"}` form. | All in-scope templates (Phi3V/Phi4MM/Fara/Qwen2.5-VL/Qwen3-VL/Qwen3.5) handle the structured form in upstream C#. If a future catalog model needs raw vision tags, address as a follow-up. |
| `flMessageData` ABI revision touches every binding (C#, Python, JS) and existing samples. | We are pre-release so the cost is acceptable. The C++ wrapper preserves the existing single-string `Item::Message` overload, so internal C++ call sites compile unchanged; bindings get a planned, one-time update. |
| Local file read introduces a path-traversal / symlink concern at the HTTP boundary. | Resolve and canonicalise paths; reject if outside an allow-list (TBD with security review). For now, gate file-path support behind the same authorisation as the rest of `/v1/responses`. |

## Effort Estimate

~850 lines of net new code (incl. tests and the `MessageItem` schema
migration).
