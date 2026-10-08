// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import com.fasterxml.jackson.annotation.JsonValue;

/** Supported typed-decision question kinds. */
public enum DecisionQuestionType {
    NOUL("noul"),
    CHOICE("choice"),
    SCORE("score");

    private final String value;

    DecisionQuestionType(String value) {
        this.value = value;
    }

    @JsonValue public String value() {
        return value;
    }
}
