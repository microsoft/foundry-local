# Tiny paged-attention test model

Repository-owned, deterministic, untrained decoder for `DynamicEngineChatTest`.
The checked-in ONNX graph and tokenizer run on CPU, without downloads, credentials,
GPU libraries, or a model cache. CMake stages them with the other `testdata` assets.

The graph implements one-head scalar causal attention using standard ONNX
`MatMul` and `Softmax` operators. It writes keys and values through the Engine's
physical block table and gathers prior logical pages back for attention. With
`Q = K = 1` and `V = token_id`, the attention result is the mean of all token IDs
in the causal history. Its deterministic next-token rule is:

```text
next_token = 3 + (round(attention * sequence_length) + 7 * sequence_length) % 95
```

This lets tests check every generated token against an independent integer
reference, including across page boundaries, retained turns, and concurrent
requests. A dropped or contaminated KV history changes the output. The printable
ASCII tokenizer is lossless (one character per token); IDs 0-2 are reserved
private-use Unicode characters. EOS is never generated, so output limits and explicit cancellation
control the turn lifetime.

The model has a 1,024-token context, four-token pages, 512 physical pages, and two
resident request slots. A 16-token scheduling budget also exercises chunked prefill.
Its graph tests Engine paging, dispatch, retention,
eviction, cancellation, and SDK accounting, **not** the numerical correctness or
performance of CUDA-specific paged-attention kernels or a trained language model.

To regenerate the assets from this directory:

```text
python -m pip install -r requirements.txt
python generate-model.py
```

Generation dependencies are not needed when building or running the C++ tests.
The generator and its generated assets are covered by the repository's MIT license.
