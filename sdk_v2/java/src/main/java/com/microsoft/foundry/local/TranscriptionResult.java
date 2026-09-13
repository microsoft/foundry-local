// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

/** Cancellation never masquerades as a successful final transcript. */
public record TranscriptionResult(String text, String language, Long durationMs,
                                  boolean cancelled, int nativeFinishReason, long elapsedMillis) {}
