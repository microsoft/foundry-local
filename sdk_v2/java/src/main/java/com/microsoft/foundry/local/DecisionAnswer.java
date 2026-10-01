// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import com.fasterxml.jackson.annotation.JsonProperty;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Map;

/** Typed answer for one decision question. */
public record DecisionAnswer(
        @JsonProperty("type") DecisionQuestionType type,
        Double noul,
        String choice,
        Double score,
        Double confidence,
        Map<String, Double> probabilities,
        Map<String, String> legend) {
    public DecisionAnswer {
        probabilities = probabilities == null
                ? Map.of()
                : Collections.unmodifiableMap(new LinkedHashMap<>(probabilities));
        legend = legend == null
                ? Map.of()
                : Collections.unmodifiableMap(new LinkedHashMap<>(legend));
    }
}
