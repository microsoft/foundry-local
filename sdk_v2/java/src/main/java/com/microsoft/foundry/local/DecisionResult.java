// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Map;

/** Result matching the {@code POST /v1/systemone} JSON contract. */
public record DecisionResult(
        String model,
        Map<String, DecisionAnswer> answers,
        DecisionUsage usage) {
    public DecisionResult {
        answers = Collections.unmodifiableMap(new LinkedHashMap<>(answers));
    }
}
