// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Objects;

/** Request matching the {@code POST /v1/systemone} JSON contract. */
public record DecisionRequest(
        Object state,
        Map<String, DecisionQuestion> questions,
        String model,
        double temperature) {

    public DecisionRequest {
        questions = Collections.unmodifiableMap(
                new LinkedHashMap<>(Objects.requireNonNull(questions, "questions")));
        model = Objects.requireNonNull(model, "model");
        if (questions.isEmpty()) throw new IllegalArgumentException("questions must not be empty");
        if (model.isEmpty()) throw new IllegalArgumentException("model must not be empty");
        if (!Double.isFinite(temperature) || temperature <= 0 || temperature > 100) {
            throw new IllegalArgumentException("temperature must be in (0, 100]");
        }
    }

    /** Creates a request with the endpoint defaults. */
    public DecisionRequest(Object state, Map<String, DecisionQuestion> questions) {
        this(state, questions, "kev", 1.0);
    }
}
