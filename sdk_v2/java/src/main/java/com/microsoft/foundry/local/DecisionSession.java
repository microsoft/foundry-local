// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

/** Session for a model whose task is {@code typed-decision}. */
public final class DecisionSession extends JsonSession {
    DecisionSession(Model model) {
        super(model, "typed-decision", "DecisionSession");
    }

    /** Evaluates the questions in {@code request}. */
    public DecisionResult decide(DecisionRequest request) {
        return process(request, DecisionResult.class);
    }
}
