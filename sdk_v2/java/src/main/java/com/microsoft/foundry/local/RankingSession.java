// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

/** Session for a model whose task is {@code text-ranking}. */
public final class RankingSession extends JsonSession {
    RankingSession(Model model) {
        super(model, "text-ranking", "RankingSession");
    }

    /** Ranks the candidate answers in {@code request}. */
    public RankingResult rank(RankingRequest request) {
        return process(request, RankingResult.class);
    }
}
