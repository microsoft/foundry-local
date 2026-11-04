// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import java.nio.file.Path;

/** Child-process fixture for the abandoned-stream shutdown test. */
public final class NativeAbandonedStream {
    private NativeAbandonedStream() {}

    public static void main(String[] args) {
        Configuration config = Configuration.builder("java-asr-shutdown")
                .runtimeDirectory(Path.of(args[0]))
                .modelCacheDirectory(Path.of(args[1]))
                .appDataDirectory(Path.of(args[2]))
                .build();
        FoundryLocalManager manager = new FoundryLocalManager(config);
        Model model = manager.catalog().getModelVariant(args[3]);
        model.load();
        model.createAudioSession().streamPcm(PcmFormat.SPEECH, event -> {});
        System.out.println("stream-started");
    }
}
