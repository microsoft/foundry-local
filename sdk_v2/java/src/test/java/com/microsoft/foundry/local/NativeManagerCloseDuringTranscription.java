// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import java.nio.file.Path;

/** Child-process fixture for manager shutdown during active transcription. */
public final class NativeManagerCloseDuringTranscription {
    private NativeManagerCloseDuringTranscription() {}

    public static void main(String[] args) {
        Configuration config = Configuration.builder("java-asr-manager-close")
                .runtimeDirectory(Path.of(args[0]))
                .modelCacheDirectory(Path.of(args[1]))
                .appDataDirectory(Path.of(args[2]))
                .build();
        FoundryLocalManager manager = new FoundryLocalManager(config);
        Model model = manager.catalog().getModelVariant(args[3]);
        model.load();
        model.createAudioSession().streamPcm(PcmFormat.SPEECH, event -> {});
        manager.close();
        System.out.println("manager-closed");
    }
}
