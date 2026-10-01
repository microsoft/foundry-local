// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import com.fasterxml.jackson.annotation.JsonProperty;
import java.util.Objects;

/** One keyed question in a {@link DecisionRequest}. */
public record DecisionQuestion(
        @JsonProperty("type") DecisionQuestionType type,
        Object criteria,
        Object instructions) {
    public DecisionQuestion {
        Objects.requireNonNull(type, "type");
    }

    /** Creates a question without optional criteria or instructions. */
    public DecisionQuestion(DecisionQuestionType type) {
        this(type, null, null);
    }
}
