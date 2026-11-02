# Speech synthesis (text to speech)

Foundry Local runs text-to-speech models with the task `text-to-speech`. The supported model family is
[VibeVoice-Realtime-0.5B](https://huggingface.co/microsoft/VibeVoice-Realtime-0.5B) (MIT) by Microsoft Research, in
the ONNX FP16 export [nenad1002/microsoft-vibevoice-0.5B-onnx-fp16](https://huggingface.co/nenad1002/microsoft-vibevoice-0.5B-onnx-fp16)
(MIT, third party). Source code: [microsoft/VibeVoice](https://github.com/microsoft/VibeVoice). Technical report:
[arXiv:2508.19205](https://arxiv.org/abs/2508.19205).

## Architecture

- ONNX Runtime GenAI runs the model. VibeVoice is an ORT GenAI model type (`vibevoice`). ORT GenAI loads the five
  graphs and the voice presets from a `genai_config.json` package and runs the generation loop. Its multimodal
  processor prepares and tokenizes the text.
- Foundry Local loads the package like all other ORT GenAI packages, through `GenAIModelInstance`. The catalog, the
  cache scanner, downloads and local registration use the same `genai_config.json` rules. For speech, Foundry Local
  reads only `model.type` and `model.sample_rate` from the configuration, and gets the voices from ORT GenAI
  (`OgaModel::GetVoices`).
- Foundry Local adds the task `text-to-speech` and `SpeechSynthesisSession`. The session checks the input and the
  options and splits the text into chunks. For each chunk, it creates one ORT GenAI generator, sets the runtime option
  `voice` and generates one speech frame per step. Then it converts the `audio` output (float samples at
  `model.sample_rate`, 24 kHz for VibeVoice) to 16-bit PCM. When a chunk is too long for the model context, the
  request fails with `INVALID_ARGUMENT`. Cancellation stops synthesis between chunks and between speech frames. It also
  stops a running step with the ORT GenAI runtime option `terminate_session`, because the last step of a chunk runs
  the vocoder over the whole chunk (up to about 3 s on a CPU).
- `SpeechSynthesisSession` uses the generic request/response API and only the public ORT GenAI API. There are no new
  C ABI functions, so the shared host and all bindings work without IPC changes.

## How VibeVoice generates speech

VibeVoice reads text tokens from the package tokenizer (Qwen2.5 byte-level BPE). The tokenizer reads control tokens
such as `<|endoftext|>` as plain text (option `literal`), so text cannot inject them. As in the VibeVoice processor,
typographic quotes become straight quotes and the text ends with a line break.

| Graph | Inputs (float16 unless noted) | Outputs |
|---|---|---|
| `text_lm` (4-layer Qwen2) | `input_ids` int64 [1, n], `past_key/value_<i>` | `hidden_states` [1, n, 896], `present_*` |
| `tts_lm` (20-layer Qwen2) | `inputs_embeds` [1, n, 896], `tts_text_mask` int64 [1, n], cache | `hidden_states`, `eos_logits` [1, 1], cache |
| `diffusion_head` | `noisy_latent` [1, 64], `timestep` [1], `condition` [1, 896] | `predicted_noise` [1, 64] (v-prediction) |
| `acoustic_connector` | `latent` [1, 1, 64] | `embedding` [1, 1, 896] |
| `vocoder` | `latents` [1, F, 64] | `audio` [1, 3200 F], 24 kHz |

A voice preset holds the key/value caches of the text LM, the TTS LM and the negative (unconditioned) TTS LM after a
voice prompt. For each chunk of text, ORT GenAI does these steps:

1. Windows of 5 text tokens go through the text LM into the TTS LM (`tts_text_mask` 1).
2. After each window, the TTS LM makes 6 speech latents. Each latent is 5 steps of multistep DPM-Solver++ over the
   diffusion head, with classifier-free guidance (scale 1.5) between the TTS LM and the negative TTS LM. The latent
   goes back into both LMs through the acoustic connector (`tts_text_mask` 0).
3. Generation stops when the EOS classifier fires after all text is fed, or at a frame budget. The budget always
   leaves room to feed all text.
4. The vocoder decodes all latents of the chunk at once (7.5 frames per second of speech).

The noise comes from a fixed seed (`search.random_seed`), so the same text and voice give the same audio on one
platform. The VibeVoice
acoustic encoder is not part of the release, so you cannot make voices from audio. Only the shipped presets exist.

## Contract

| | |
|---|---|
| Input | TEXT items. Each item, and each line in it, is a paragraph. At most 16384 characters in total. |
| Output | One AUDIO item: signed 16-bit little-endian PCM (`pcm`), mono, at `model.sample_rate` (24 kHz for VibeVoice). Metadata `voice`. |
| Streaming | One AUDIO item per chunk. Metadata `text` is the chunk. A chunk is a sentence. A sentence shorter than 24 characters is joined with the next sentence of its line. A sentence longer than 400 characters is split at clause punctuation, then between words. |
| Usage | `prompt_tokens` = text tokens, `completion_tokens` = speech frames. |
| Options | `voice` (`FOUNDRY_LOCAL_PARAM_VOICE`, a name in `model.voices`, default `model.default_voice`). `speed` (`FOUNDRY_LOCAL_PARAM_SPEED`) accepts only 1.0, because VibeVoice has no rate control. |

## Quality, speed and memory

Measured end to end through Foundry Local on an Apple Silicon Mac, CPU execution provider, ONNX Runtime 1.31:

- Whisper (base.en) transcribed sentences of 5 to 20 words correctly for all seven English voices (apart from
  single-word confusions such as "bill" for "build"). Text of one to three words can be unreliable: with a different
  noise seed, Carter read "OK, thanks." as "Have a good day." and Mike made no clear speech for "Hi.". This is why
  the session joins short sentences. Emma read short text correctly in all tests, so Emma is the default.
- Synthesis runs at about 0.4 of real time: a chunk of 44 characters takes 1.2 to 1.8 s, and 135 characters in two
  chunks take 4.2 to 5.0 s. The vocoder decodes a chunk at the end, so the first audio of a chunk arrives when the
  whole chunk is ready. Loading takes 3 to 5.6 s.
- The package is 1.7 GB. The process peaks at 4.2 to 4.8 GB resident memory: full graph optimization converts the
  float16 weights for the CPU. With basic optimization the CPU converts them on every run instead, which is 3 to 5
  times slower and uses more memory at run time.
- The package turns off intra-op spinning (`session.intra_op.allow_spinning` 0). The latency stays the same, and the
  CPU time decreases to a third (11.8 s instead of 38.1 s of user time for the same requests).

## Model package

A package is a directory with a `genai_config.json`, like all other ORT GenAI packages:

```text
genai_config.json           ORT GenAI configuration, model type "vibevoice" (below)
text_lm_kv.onnx             graphs of the ONNX FP16 export; .onnx.data files stay next to their graphs
tts_lm_kv.onnx
diffusion_head.onnx (+ .data)
acoustic_connector.onnx (+ .data)
vocoder.onnx (+ .data)
tokenizer.json              Qwen2.5 tokenizer
tokenizer_config.json
voices/<name>.onnx          voice presets: ONNX graphs without inputs whose outputs are the cached voice prompt
MODEL_CARD.md               model card of the export (license MIT)
```

The configuration takes its numbers from `config.json` and `schedule.json` of the export, and the voice presets are
converted from the export's `.npz` files. The ORT GenAI script `examples/python/vibevoice-package.py` writes the
package from a download of the export:

```json
{
  "model": {
    "type": "vibevoice",
    "context_length": 8192,
    "vocab_size": 151936,
    "bos_token_id": 151643,
    "eos_token_id": 151643,
    "pad_token_id": 151643,
    "sample_rate": 24000,
    "decoder": {"session_options": {"session.intra_op.allow_spinning": "0", "provider_options": []}},
    "default_voice": "Emma",
    "voices": [
      {"name": "Emma", "file": "voices/en-Emma_woman.onnx", "language": "en", "gender": "female"},
      {"name": "Grace", "file": "voices/en-Grace_woman.onnx", "language": "en", "gender": "female"},
      {"name": "Carter", "file": "voices/en-Carter_man.onnx", "language": "en", "gender": "male"},
      {"name": "Davis", "file": "voices/en-Davis_man.onnx", "language": "en", "gender": "male"},
      {"name": "Frank", "file": "voices/en-Frank_man.onnx", "language": "en", "gender": "male"},
      {"name": "Mike", "file": "voices/en-Mike_man.onnx", "language": "en", "gender": "male"},
      {"name": "Samuel", "file": "voices/in-Samuel_man.onnx", "language": "en-in", "gender": "male"}
    ],
    "vibevoice": {
      "text_lm": {"filename": "text_lm_kv.onnx"},
      "tts_lm": {"filename": "tts_lm_kv.onnx"},
      "diffusion_head": {"filename": "diffusion_head.onnx"},
      "acoustic_connector": {"filename": "acoustic_connector.onnx"},
      "vocoder": {"filename": "vocoder.onnx"},
      "speech_scaling_factor": 0.2333984375,
      "speech_bias_factor": -0.0703125,
      "text_window_size": 5,
      "speech_window_size": 6,
      "cfg_scale": 1.5,
      "diffusion": {"timesteps": [999, 799, 599, 400, 200],
                    "sigmas": [20291.302734375, 3.103729724884033, 1.3907514810562134, 0.7402812242507935,
                               0.3374606668949127, 0.0]}
    }
  },
  "search": {"random_seed": 24301}
}
```

- `model.voices` lists the voice presets. `language` is a BCP 47 tag and `gender` is optional. `model.default_voice`
  is the voice when a request has no `voice` option. Without it, the first voice is the default. The Rust binding reads
  the voices from this file, so it can list them before the model loads (`Model::speech_voices`). The native session
  gets them from the loaded model through ORT GenAI.
- `model.vibevoice` holds the graph files, the latent scale and bias for the vocoder, the window sizes, the guidance
  scale and the DPM-Solver++ schedule (one timestep per step, one sigma more than steps). `text_window_size`,
  `speech_window_size` and `cfg_scale` are optional.

The export also has presets for nine other languages. The package does not include them, because the model card
supports English only. The export's model card lists a voice "Wayne", but neither the export nor microsoft/VibeVoice
has it.

## Sources and licenses

The converted files were checked against their sources: the graph inputs and outputs match the table above, and the
voice presets equal the BF16 presets in microsoft/VibeVoice `demo/voices/streaming_model` (largest difference 3e-8).
The export, the VibeVoice code and the model are MIT. The Qwen2.5 tokenizer is Apache-2.0.

## Catalog and local models

A catalog entry needs `inferenceTasks: ["text-to-speech"]`, the package directory as the downloadable asset
(`genai_config.json` at the root or in one sub-directory) and `minFLVersion` set to the first release with speech
synthesis. To use a local package without a catalog, register it in the local catalog with task `text-to-speech`
(for example Rust `catalog.register_model(path, "vibevoice-realtime-0.5b:1", metadata)`).

## Tests

`test/internal_api/speech_synthesis/` and `sdk_v2/rust/tests/integration/speech_test.rs` use the deterministic
`test/testdata/tiny-vibevoice` package: five tiny float16 graphs with the VibeVoice signatures, a byte-level tokenizer
and two voices (no download). It is a copy of the ORT GenAI test model `test/models/vibevoice`; see its README. The
generation loop and the solver have their tests in ORT GenAI.
