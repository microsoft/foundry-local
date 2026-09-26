// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

/** NONE is a token delta in this runtime, not a genuine partial utterance hypothesis. */
public record SpeechEvent(Kind kind, String text, Long startTimeMs, Long endTimeMs, boolean utteranceStart) {
    public enum Kind { TOKEN, PARTIAL, FINAL }
}
