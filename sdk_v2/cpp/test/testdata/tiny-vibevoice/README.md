# tiny-vibevoice

A deterministic, untrained stand-in for a VibeVoice-Realtime package, for the speech synthesis tests (no download).
It needs an ONNX Runtime GenAI build with the model type `vibevoice`.

The files are a copy of `test/models/vibevoice` in microsoft/onnxruntime-genai, without the invalid-voice fixture
`voices/two_heads.onnx`. Regenerate them there with `python test/python/create/create_dummy_vibevoice_models.py
--output test/models/vibevoice` and copy them here.

The tests rely on these properties: a byte tokenizer (token id = UTF-8 byte), voices Alpha (default, TTS prompt of
3 positions, negative condition 0.05) and Beta (5 positions, -0.05), four audio samples per speech frame, and an end of
speech when the TTS cache is longer than 40 positions.
