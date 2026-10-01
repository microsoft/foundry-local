// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import java.util.List;
import java.util.Objects;

/** Request matching the {@code POST /v1/rank} JSON contract. */
public record RankingRequest(
        Object context,
        String question,
        List<String> answers,
        String model,
        double temperature) {

    public RankingRequest {
        question = Objects.requireNonNull(question, "question");
        answers = List.copyOf(Objects.requireNonNull(answers, "answers"));
        model = Objects.requireNonNull(model, "model");
        if (answers.isEmpty() || answers.stream().anyMatch(String::isEmpty)) {
            throw new IllegalArgumentException("answers must contain non-empty strings");
        }
        if (model.isEmpty()) throw new IllegalArgumentException("model must not be empty");
        if (!Double.isFinite(temperature) || temperature <= 0 || temperature > 100) {
            throw new IllegalArgumentException("temperature must be in (0, 100]");
        }
    }

    /** Creates a request with the endpoint defaults. */
    public RankingRequest(Object context, String question, List<String> answers) {
        this(context, question, answers, "clm", 1.0);
    }
}
