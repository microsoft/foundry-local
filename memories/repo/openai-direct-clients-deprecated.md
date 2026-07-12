# OpenAI direct clients are deprecated — use Session API

The OpenAI **direct** clients (OpenAI types, but native `*-json` pass-through instead of the web service) are deprecated in all SDKs. Do not extend them; route users to the Session API.

Migration:
- `OpenAIChatClient` / `ChatClient` → `ChatSession`
- `OpenAIAudioClient` / `AudioClient` → `AudioSession`
- `OpenAIEmbeddingClient` / `EmbeddingClient` → `EmbeddingsSession`
- `LiveAudioTranscriptionSession` (the OpenAI variant) → `AudioSession` streaming
- Factories `Get*ClientAsync` / `create*Client` / `get_*_client` are also deprecated.

OpenAI types themselves stay supported for the web-server path. Deprecation: C# `[Obsolete(error:false)]`, JS `@deprecated` JSDoc, Python `typing_extensions.deprecated`.
