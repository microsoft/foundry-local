// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Objects;

/** Detached immutable snapshot of one model variant's catalog metadata. */
public final class ModelInfo {
    private final String id;
    private final String alias;
    private final String name;
    private final int version;
    private final String uri;
    private final DeviceType deviceType;
    private final String executionProvider;
    private final String task;
    private final boolean cached;
    private final Map<String, String> stringProperties;
    private final Map<String, Long> intProperties;
    private final Map<String, String> modelSettings;

    ModelInfo(
            String id,
            String alias,
            String name,
            int version,
            String uri,
            DeviceType deviceType,
            String executionProvider,
            String task,
            boolean cached,
            Map<String, String> stringProperties,
            Map<String, Long> intProperties,
            Map<String, String> modelSettings) {
        this.id = id;
        this.alias = alias;
        this.name = name;
        this.version = version;
        this.uri = uri;
        this.deviceType = deviceType;
        this.executionProvider = executionProvider;
        this.task = task;
        this.cached = cached;
        this.stringProperties = Map.copyOf(stringProperties);
        this.intProperties = Map.copyOf(intProperties);
        this.modelSettings = java.util.Collections.unmodifiableMap(new LinkedHashMap<>(modelSettings));
    }

    public String id() { return id; }
    public String alias() { return alias; }
    public String name() { return name; }
    public int version() { return version; }
    public String uri() { return uri; }
    public DeviceType deviceType() { return deviceType; }
    public String executionProvider() { return executionProvider; }
    public String task() { return task; }
    public boolean cached() { return cached; }
    public Map<String, String> stringProperties() { return stringProperties; }
    public Map<String, Long> intProperties() { return intProperties; }
    public Map<String, String> modelSettings() { return modelSettings; }

    public String getStringProperty(String key) {
        return stringProperties.get(validateKey(key));
    }

    public long getIntProperty(String key, long defaultValue) {
        return intProperties.getOrDefault(validateKey(key), defaultValue);
    }

    public String displayName() { return getStringProperty(ModelProperties.DISPLAY_NAME); }
    public String modelType() { return getStringProperty(ModelProperties.MODEL_TYPE); }
    public String publisher() { return getStringProperty(ModelProperties.PUBLISHER); }
    public String license() { return getStringProperty(ModelProperties.LICENSE); }
    public String licenseDescription() { return getStringProperty(ModelProperties.LICENSE_DESCRIPTION); }
    public String modelProvider() { return getStringProperty(ModelProperties.MODEL_PROVIDER); }
    public String minFlVersion() { return getStringProperty(ModelProperties.MIN_FL_VERSION); }
    public String parentUri() { return getStringProperty(ModelProperties.PARENT_URI); }
    public String toolCallStart() { return getStringProperty(ModelProperties.TOOL_CALL_START); }
    public String toolCallEnd() { return getStringProperty(ModelProperties.TOOL_CALL_END); }
    public String reasoningStart() { return getStringProperty(ModelProperties.REASONING_START); }
    public String reasoningEnd() { return getStringProperty(ModelProperties.REASONING_END); }
    public String inputModalities() { return getStringProperty(ModelProperties.INPUT_MODALITIES); }
    public String outputModalities() { return getStringProperty(ModelProperties.OUTPUT_MODALITIES); }
    public String capabilities() { return getStringProperty(ModelProperties.CAPABILITIES); }
    public Boolean supportsToolCalling() { return booleanProperty(ModelProperties.SUPPORTS_TOOL_CALLING); }
    public Boolean supportsReasoning() { return booleanProperty(ModelProperties.SUPPORTS_REASONING); }
    public Boolean supportsHybridReasoning() {
        return booleanProperty(ModelProperties.SUPPORTS_HYBRID_REASONING);
    }
    public Long fileSizeMb() { return intProperties.get(ModelProperties.FILE_SIZE_MB); }
    public Long maxOutputTokens() { return intProperties.get(ModelProperties.MAX_OUTPUT_TOKENS); }
    public Long contextLength() { return intProperties.get(ModelProperties.CONTEXT_LENGTH); }
    public long createdAtUnix() { return getIntProperty(ModelProperties.CREATED_AT_UNIX, 0); }
    public boolean testModel() { return getIntProperty(ModelProperties.IS_TEST_MODEL, 0) != 0; }

    private static String validateKey(String key) {
        Objects.requireNonNull(key, "key");
        if (key.isEmpty() || key.indexOf('\0') >= 0) {
            throw new IllegalArgumentException("Property key must be nonempty and contain no NUL");
        }
        return key;
    }

    private Boolean booleanProperty(String key) {
        Long value = intProperties.get(key);
        return value == null || value < 0 ? null : value != 0;
    }
}
