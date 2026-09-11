// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import static org.junit.jupiter.api.Assertions.*;
import static org.junit.jupiter.api.Assumptions.assumeTrue;
import java.nio.file.Path;
import java.time.Duration;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

/** Explicit opt-in; never downloads a model. All sessions share the caller's prepared cache. */
class NativeAsrTest {
    @TempDir Path temporary;

    @Test void realAsrOwnershipCancellationAndRepeatedSessions() throws Exception {
        String runtime = System.getProperty("foundry.test.runtime");
        String cache = System.getProperty("foundry.test.cache");
        String wav = System.getProperty("foundry.test.wav");
        assumeTrue(runtime != null && cache != null && wav != null, "Set the three foundry.test.* properties");
        Configuration config = new Configuration("java-asr-test", Path.of(runtime), Path.of(cache), temporary);
        Model borrowed;
        AtomicInteger callbacks = new AtomicInteger();
        try (FoundryLocalManager manager = new FoundryLocalManager(config)) {
            System.err.println("native-test: manager created");
            assertThrows(IllegalStateException.class, () -> new FoundryLocalManager(config));
            assertThrows(IllegalArgumentException.class, () -> manager.catalog().getModel("nemotron"));
            borrowed = manager.catalog().getModel(System.getProperty("foundry.test.model",
                    "nemotron-3.5-asr-streaming-0.6b-generic-cpu:3"));
            CancellationToken cancelled = new CancellationToken();
            cancelled.cancel();
            FoundryLocalException download = assertThrows(FoundryLocalException.class,
                    () -> borrowed.download(cancelled, ignored -> fail("Must not invoke progress")));
            assertEquals(5, download.code());
            assertTrue(borrowed.isCached(), "Explicitly prepare the model first");
            borrowed.load();
            System.err.println("native-test: model loaded");
            byte[] pcm = WavAudio.read(Path.of(wav)).pcm();
            try (AudioSession session = borrowed.createAudioSession();
                 Transcription run = session.transcribeWav(Path.of(wav), event -> callbacks.incrementAndGet())) {
                assertFalse(run.await(Duration.ofSeconds(60)).text().isBlank());
                assertEquals(pcm.length, run.timing().submittedBytes());
                assertNotNull(run.timing().inputClosedMillis());
            }
            System.err.println("native-test: WAV finished and closed");
            for (int iteration = 0; iteration < 2; iteration++) {
                try (AudioSession session = borrowed.createAudioSession()) {
                    assertThrows(IllegalStateException.class, borrowed::unload);
                    try (Transcription run = session.streamPcm(PcmFormat.SPEECH, event -> callbacks.incrementAndGet())) {
                        for (int offset = 0; offset < pcm.length; offset += 3200) {
                            run.writePcm(Arrays.copyOfRange(pcm, offset, Math.min(offset + 3200, pcm.length)));
                        }
                        run.finishInput();
                        TranscriptionResult result = run.await(Duration.ofMinutes(3));
                        assertFalse(result.cancelled());
                        assertEquals(2, result.nativeFinishReason());
                        assertFalse(result.text().isBlank());
                        assertThrows(IllegalStateException.class, () -> run.writePcm(new byte[2]));
                    }
                    System.err.println("native-test: PCM finished and closed " + iteration);
                    int count = callbacks.get();
                    Thread.sleep(100);
                    assertEquals(count, callbacks.get(), "No callback may outlive close");
                    try (Transcription run = session.streamPcm(PcmFormat.SPEECH, event -> callbacks.incrementAndGet())) {
                        run.writePcm(Arrays.copyOf(pcm, Math.min(3200, pcm.length)));
                        run.cancel();
                        assertTrue(run.await(Duration.ofSeconds(30)).cancelled());
                        assertTrue(run.isCancelled());
                    }
                    System.err.println("native-test: cancellation acknowledged and closed " + iteration);
                    try (Transcription run = session.streamPcm(PcmFormat.SPEECH, event -> {
                        throw new IllegalStateException("listener failure");
                    })) {
                        for (int offset = 0; offset < Math.min(pcm.length, 64000); offset += 3200) {
                            if (run.isDone()) break;
                            run.writePcm(Arrays.copyOfRange(pcm, offset, Math.min(offset + 3200, pcm.length)));
                        }
                        run.finishInput();
                        assertThrows(IllegalStateException.class, () -> run.await(Duration.ofSeconds(30)));
                    }
                    System.err.println("native-test: callback failure surfaced and closed " + iteration);
                }
            }
            // Manager owns and closes an outstanding session/request even if the caller forgets.
            borrowed.createAudioSession().streamPcm(PcmFormat.SPEECH, event -> callbacks.incrementAndGet());
            System.err.println("native-test: cascade close starting");
        }
        System.err.println("native-test: manager closed");
        assertThrows(IllegalStateException.class, borrowed::isLoaded);
        int count = callbacks.get();
        Thread.sleep(100);
        assertEquals(count, callbacks.get());
        assertTrue(Thread.getAllStackTraces().keySet().stream()
                .noneMatch(thread -> thread.isAlive() && thread.getName().startsWith("foundry-java-asr")));
        try (FoundryLocalManager manager = new FoundryLocalManager(config)) {
            assertFalse(manager.runtimeVersion().isBlank());
        }
        System.err.println("native-test: manager recreated and closed");
    }
}
