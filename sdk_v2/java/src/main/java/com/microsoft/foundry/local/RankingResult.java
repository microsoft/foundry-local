// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import java.util.List;

/** Result matching the {@code POST /v1/rank} JSON contract. */
public record RankingResult(String model, List<RankedCandidate> ranked) {
    public RankingResult {
        ranked = List.copyOf(ranked);
    }
}
