// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

/** Observed request-local monotonic timing, not model timestamps or download byte counters. */
public record TranscriptionTiming(Double firstInputMillis, Double firstNonemptyMillis,
                                  Double inputClosedMillis, Double finalizedMillis,
                                  Double cancellationRequestedMillis, long submittedBytes) {}
