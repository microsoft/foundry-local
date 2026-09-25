// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

/** Well-known model metadata keys. The native API also permits arbitrary keys for local model registration. */
public final class ModelProperties {
    public static final String DISPLAY_NAME = "display_name";
    public static final String MODEL_TYPE = "type";
    public static final String PUBLISHER = "publisher";
    public static final String LICENSE = "license";
    public static final String LICENSE_DESCRIPTION = "license_description";
    public static final String TASK = "task";
    public static final String MODEL_PROVIDER = "model_provider";
    public static final String MIN_FL_VERSION = "min_fl_version";
    public static final String PARENT_URI = "parent_uri";
    public static final String TOOL_CALL_START = "tool_call_start";
    public static final String TOOL_CALL_END = "tool_call_end";
    public static final String REASONING_START = "reasoning_start";
    public static final String REASONING_END = "reasoning_end";
    public static final String DEVICE_TYPE = "device_type";
    public static final String EXECUTION_PROVIDER = "execution_provider";
    public static final String ENTITY_TYPE = "entity_type";
    public static final String AUTHOR = "author";
    public static final String QUANTIZATION = "quantization";
    public static final String CREATION_TIME = "creation_time";
    public static final String INPUT_MODALITIES = "input_modalities";
    public static final String OUTPUT_MODALITIES = "output_modalities";
    public static final String CAPABILITIES = "capabilities";

    public static final String SUPPORTS_TOOL_CALLING = "supports_tool_calling";
    public static final String SUPPORTS_REASONING = "supports_reasoning";
    public static final String FILE_SIZE_MB = "filesize_mb";
    public static final String MAX_OUTPUT_TOKENS = "max_output_tokens";
    public static final String CREATED_AT_UNIX = "created_at_unix";
    public static final String IS_TEST_MODEL = "is_test_model";
    public static final String CONTEXT_LENGTH = "context_length";
    public static final String SUPPORTS_HYBRID_REASONING = "supports_hybrid_reasoning";

    static final String[] STRING_KEYS = {
        DISPLAY_NAME, MODEL_TYPE, PUBLISHER, LICENSE, LICENSE_DESCRIPTION, TASK, MODEL_PROVIDER,
        MIN_FL_VERSION, PARENT_URI, TOOL_CALL_START, TOOL_CALL_END, REASONING_START, REASONING_END,
        DEVICE_TYPE, EXECUTION_PROVIDER, ENTITY_TYPE, AUTHOR, QUANTIZATION, CREATION_TIME,
        INPUT_MODALITIES, OUTPUT_MODALITIES, CAPABILITIES
    };

    static final String[] INT_KEYS = {
        SUPPORTS_TOOL_CALLING, SUPPORTS_REASONING, FILE_SIZE_MB, MAX_OUTPUT_TOKENS, CREATED_AT_UNIX,
        IS_TEST_MODEL, CONTEXT_LENGTH, SUPPORTS_HYBRID_REASONING
    };

    private ModelProperties() {}
}
