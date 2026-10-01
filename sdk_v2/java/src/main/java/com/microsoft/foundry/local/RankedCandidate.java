// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

/** One ranked candidate from a {@link RankingResult}. */
public record RankedCandidate(long rank, String candidate, double prob) {}
