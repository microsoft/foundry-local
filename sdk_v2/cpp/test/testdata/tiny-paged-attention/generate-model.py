# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
"""Regenerate the deterministic CPU paged-attention fixture; not needed to run tests."""

import json
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, helper
from onnx.reference import ReferenceEvaluator
from tokenizers import Tokenizer, decoders, models, pre_tokenizers


BLOCK_SIZE = 4
NUM_BLOCKS = 512
CONTEXT_LENGTH = 1024
VOCAB_SIZE = 98
SPECIAL_TOKENS = ["\ue000", "\ue001", "\ue002"]
ROOT = Path(__file__).resolve().parent


def create_model():
    nodes = []
    initializers = []

    def constant(name, values, dtype=TensorProto.INT64, scalar=False):
        initializers.append(helper.make_tensor(name, dtype, [] if scalar else [len(values)], values))
        return name

    def node(op, inputs, output, **attrs):
        nodes.append(helper.make_node(op, inputs, [output], name=output, **attrs))
        return output

    zero = constant("zero", [0], scalar=True)
    one = constant("one", [1], scalar=True)
    seven = constant("seven", [7], scalar=True)
    first_character = constant("first_character", [3], scalar=True)
    character_count = constant("character_count", [95], scalar=True)
    block_size = constant("block_size", [BLOCK_SIZE], scalar=True)
    vocab_size = constant("vocab_size", [VOCAB_SIZE], scalar=True)
    axis0 = constant("axis0", [0])
    axis1 = constant("axis1", [1])
    axis2 = constant("axis2", [2])
    axes12 = constant("axes12", [1, 2])
    starts0 = constant("starts0", [0])
    starts1 = constant("starts1", [1])
    end_minus1 = constant("end_minus1", [-1])
    end_max = constant("end_max", [2**63 - 1])
    flat_shape = constant("flat_shape", [-1])
    cache_shape = constant("cache_shape", [NUM_BLOCKS, BLOCK_SIZE, 1, 1])
    zero_float = constant("zero_float", [0.0], TensorProto.FLOAT, scalar=True)
    masked_score = constant("masked_score", [-1.0e9], TensorProto.FLOAT, scalar=True)
    one_hot_values = constant("one_hot_values", [0.0, 20.0], TensorProto.FLOAT)

    input_shape = node("Shape", ["input_ids"], "input_shape")
    token_count = node("Gather", [input_shape, zero], "token_count")
    token_index = node("Range", [zero, token_count, one], "token_index")
    boundaries = node("Cast", ["cumulative_sequence_lengths"], "boundaries", to=TensorProto.INT64)
    starts = node("Slice", [boundaries, starts0, end_minus1, axis0], "starts")
    ends = node("Slice", [boundaries, starts1, end_max, axis0], "ends")
    token_column = node("Unsqueeze", [token_index, axis1], "token_column")
    end_row = node("Unsqueeze", [ends, axis0], "end_row")
    after_end = node("GreaterOrEqual", [token_column, end_row], "after_end")
    after_end_int = node("Cast", [after_end], "after_end_int", to=TensorProto.INT64)
    request_row = node("ReduceSum", [after_end_int, axis1], "request_row", keepdims=0)
    row_start = node("Gather", [starts, request_row], "row_start")
    past_lengths = node("Cast", ["past_sequence_lengths"], "past_lengths", to=TensorProto.INT64)
    row_past = node("Gather", [past_lengths, request_row], "row_past")
    local_position = node("Sub", [token_index, row_start], "local_position")
    position = node("Add", [local_position, row_past], "position")
    length = node("Add", [position, one], "length")
    blocks = node("Cast", ["block_table"], "blocks", to=TensorProto.INT64)
    block_column = node("Div", [position, block_size], "block_column")
    row_column = node("Unsqueeze", [request_row, axis1], "row_column")
    block_column_2d = node("Unsqueeze", [block_column, axis1], "block_column_2d")
    block_indices = node("Concat", [row_column, block_column_2d], "block_indices", axis=1)
    physical_block = node("GatherND", [blocks, block_indices], "physical_block")
    block_offset = node("Mul", [physical_block, block_size], "block_offset")
    in_block = node("Mod", [position, block_size], "in_block")
    physical_slot = node("Add", [block_offset, in_block], "physical_slot")
    write_indices = node("Unsqueeze", [physical_slot, axis1], "write_indices")

    # Q = K = 1 and V = token ID give exact, history-sensitive uniform causal attention.
    keys = node(
        "ConstantOfShape",
        [input_shape],
        "keys",
        value=helper.make_tensor("key_value", TensorProto.FLOAT, [1], [1.0]),
    )
    values = node("Cast", ["input_ids"], "values", to=TensorProto.FLOAT)
    cache_outputs = {}
    for kind, updates in (("key", keys), ("value", values)):
        flat = node("Reshape", [f"past_key_values.0.{kind}", flat_shape], f"flat_{kind}")
        written = node("ScatterND", [flat, write_indices, updates], f"written_{kind}")
        node("Reshape", [written, cache_shape], f"present.0.{kind}")
        cache_outputs[kind] = written

    max_length = node("ReduceMax", [length], "max_length", keepdims=0)
    logical_slots = node("Range", [zero, max_length, one], "logical_slots")
    logical_blocks = node("Div", [logical_slots, block_size], "logical_blocks")
    read_blocks = node("Gather", [blocks, logical_blocks], "read_blocks", axis=1)
    # Unused entries in shorter requests' block tables are masked by causal attention.
    safe_blocks = node("Max", [read_blocks, zero], "safe_blocks")
    read_offsets = node("Mul", [safe_blocks, block_size], "read_offsets")
    read_in_block = node("Mod", [logical_slots, block_size], "read_in_block")
    read_slots = node("Add", [read_offsets, read_in_block], "read_slots")
    token_read_slots = node("Gather", [read_slots, request_row], "token_read_slots")
    cached_keys = node("Gather", [cache_outputs["key"], token_read_slots], "cached_keys")
    cached_values = node("Gather", [cache_outputs["value"], token_read_slots], "cached_values")
    queries = node("Unsqueeze", [keys, axes12], "queries")
    key_rows = node("Unsqueeze", [cached_keys, axis1], "key_rows")
    scores = node("MatMul", [queries, key_rows], "scores")
    position_column = node("Unsqueeze", [position, axis1], "position_column")
    causal = node("LessOrEqual", [logical_slots, position_column], "causal")
    causal_3d = node("Unsqueeze", [causal, axis1], "causal_3d")
    masked = node("Where", [causal_3d, scores, masked_score], "masked")
    weights = node("Softmax", [masked], "weights", axis=-1)
    # Zero attention weights do not suppress NaNs in unwritten future KV slots.
    causal_values = node("Where", [causal, cached_values, zero_float], "causal_values")
    value_columns = node("Unsqueeze", [causal_values, axis2], "value_columns")
    attention = node("MatMul", [weights, value_columns], "attention")
    mean = node("Squeeze", [attention, axes12], "mean")
    length_float = node("Cast", [length], "length_float", to=TensorProto.FLOAT)
    total_float = node("Mul", [mean, length_float], "total_float")
    total_rounded = node("Round", [total_float], "total_rounded")
    total = node("Cast", [total_rounded], "total", to=TensorProto.INT64)
    length_bias = node("Mul", [length, seven], "length_bias")
    seed = node("Add", [total, length_bias], "seed")
    character = node("Mod", [seed, character_count], "character")
    next_token = node("Add", [character, first_character], "next_token")
    per_token_logits = node("OneHot", [next_token, vocab_size, one_hot_values], "per_token_logits")
    node("Cast", [per_token_logits], "logits", to=TensorProto.FLOAT16)
    hidden_float16 = node("Cast", [values], "hidden_float16", to=TensorProto.FLOAT16)
    node("Unsqueeze", [hidden_float16, axis1], "hidden_states")

    def tensor(name, dtype, shape):
        return helper.make_tensor_value_info(name, dtype, shape)

    cache_dimensions = [NUM_BLOCKS, BLOCK_SIZE, 1, 1]
    inputs = [
        tensor("input_ids", TensorProto.INT64, ["num_tokens"]),
        tensor("cumulative_sequence_lengths", TensorProto.INT32, ["batch_plus_1"]),
        tensor("past_sequence_lengths", TensorProto.INT32, ["batch"]),
        tensor("block_table", TensorProto.INT32, ["batch", "max_blocks"]),
        tensor("attention_metadata", TensorProto.INT32, [3]),
        tensor("past_key_values.0.key", TensorProto.FLOAT, cache_dimensions),
        tensor("past_key_values.0.value", TensorProto.FLOAT, cache_dimensions),
    ]
    outputs = [
        tensor("logits", TensorProto.FLOAT16, ["num_tokens", VOCAB_SIZE]),
        tensor("hidden_states", TensorProto.FLOAT16, ["num_tokens", 1]),
        tensor("present.0.key", TensorProto.FLOAT, cache_dimensions),
        tensor("present.0.value", TensorProto.FLOAT, cache_dimensions),
    ]
    graph = helper.make_graph(nodes, "tiny-paged-attention", inputs, outputs, initializers)
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)], ir_version=9)
    model.producer_name = "foundry-local-test-fixtures"
    onnx.checker.check_model(model, full_check=True)
    check_poisoned_cache(model)
    onnx.save(model, ROOT / "decoder.onnx")


def check_poisoned_cache(model):
    evaluator = ReferenceEvaluator(model)
    inputs = {
        "input_ids": np.array([20, 30, 40, 50], dtype=np.int64),
        "cumulative_sequence_lengths": np.array([0, 1, 4], dtype=np.int32),
        "past_sequence_lengths": np.array([0, 0], dtype=np.int32),
        "block_table": np.array([[7], [2]], dtype=np.int32),
        "attention_metadata": np.zeros(3, dtype=np.int32),
        "past_key_values.0.key": np.full((NUM_BLOCKS, BLOCK_SIZE, 1, 1), np.nan, dtype=np.float32),
        "past_key_values.0.value": np.full((NUM_BLOCKS, BLOCK_SIZE, 1, 1), np.nan, dtype=np.float32),
    }
    outputs = evaluator.run(None, inputs)
    np.testing.assert_array_equal(np.argmax(outputs[0], axis=-1), [30, 40, 87, 49])

    inputs.update(
        {
            "input_ids": np.array([60], dtype=np.int64),
            "cumulative_sequence_lengths": np.array([0, 1], dtype=np.int32),
            "past_sequence_lengths": np.array([1], dtype=np.int32),
            "block_table": np.array([[7]], dtype=np.int32),
            "past_key_values.0.key": outputs[2],
            "past_key_values.0.value": outputs[3],
        }
    )
    np.testing.assert_array_equal(np.argmax(evaluator.run(None, inputs)[0], axis=-1), [97])


def create_tokenizer():
    vocab = {token: index for index, token in enumerate(SPECIAL_TOKENS)}
    vocab.update({chr(code): code - 32 + 3 for code in range(32, 127)})
    tokenizer = Tokenizer(models.BPE(vocab, merges=[], unk_token=SPECIAL_TOKENS[2]))
    tokenizer.pre_tokenizer = pre_tokenizers.Split("", behavior="isolated")
    tokenizer.decoder = decoders.Fuse()
    tokenizer.add_special_tokens(SPECIAL_TOKENS)
    printable = "".join(chr(code) for code in range(32, 127))
    assert tokenizer.decode(tokenizer.encode(printable).ids) == printable
    write_json("tokenizer.json", json.loads(tokenizer.to_str()))
    write_json(
        "tokenizer_config.json",
        {
            "tokenizer_class": "PreTrainedTokenizerFast",
            "bos_token": SPECIAL_TOKENS[0],
            "pad_token": SPECIAL_TOKENS[0],
            "eos_token": SPECIAL_TOKENS[1],
            "unk_token": SPECIAL_TOKENS[2],
            "model_max_length": CONTEXT_LENGTH,
            "chat_template": (
                "{% for message in messages %}"
                "{{ '[' + message['role'] + ']' + message['content'] }}"
                "{% endfor %}"
                "{% if add_generation_prompt %}{{ '[assistant]' }}{% endif %}"
            ),
        },
    )


def write_json(name, value):
    (ROOT / name).write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8", newline="\n")


if __name__ == "__main__":
    create_model()
    create_tokenizer()
    write_json(
        "genai_config.json",
        {
            "model": {
                "type": "decoder",
                "bos_token_id": 0,
                "eos_token_id": 1,
                "pad_token_id": 0,
                "vocab_size": VOCAB_SIZE,
                "context_length": CONTEXT_LENGTH,
                "decoder": {
                    "filename": "decoder.onnx",
                    "hidden_size": 1,
                    "head_size": 1,
                    "num_attention_heads": 1,
                    "num_key_value_heads": 1,
                    "num_hidden_layers": 1,
                    "session_options": {"provider_options": []},
                    "inputs": {
                        "input_ids": "input_ids",
                        "block_table": "block_table",
                        "cumulative_sequence_lengths": "cumulative_sequence_lengths",
                        "past_sequence_lengths": "past_sequence_lengths",
                        "attention_metadata": "attention_metadata",
                        "past_key_names": "past_key_values.%d.key",
                        "past_value_names": "past_key_values.%d.value",
                    },
                    "outputs": {
                        "logits": "logits",
                        "hidden_states": "hidden_states",
                        "present_key_names": "present.%d.key",
                        "present_value_names": "present.%d.value",
                    },
                    "state_groups": [{"kind": "paged_kv", "layer_ids": [0]}],
                },
            },
            "search": {"max_length": CONTEXT_LENGTH, "do_sample": False},
            "engine": {
                "dynamic_batching": {
                    "block_size": BLOCK_SIZE,
                    "num_blocks": NUM_BLOCKS,
                    "max_batch_size": 2,
                    "max_scheduled_tokens": 16,
                }
            },
        },
    )
