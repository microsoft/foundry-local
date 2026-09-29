// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import static org.junit.jupiter.api.Assertions.*;
import static org.junit.jupiter.api.Assumptions.assumeTrue;
import java.nio.file.Path;
import java.time.Duration;
import java.util.Arrays;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

/** Opt-in language qualification against a caller-prepared multilingual model and speech fixture. */
class NativeAudioLanguageTest {
    @TempDir Path temporary;

    @Test void languageHintsReachPcmAndWavRequests() throws Exception {
        String language = setting("foundry.test.language", "FOUNDRY_TEST_LANGUAGE");
        assumeTrue(language != null, "Configure a fixed language and matching speech fixture");
        assertNotEquals("auto", language, "Use a fixed language; auto is tested separately");
        String expected = required("foundry.test.language.expected", "FOUNDRY_TEST_LANGUAGE_EXPECTED");
        Path wav = Path.of(required("foundry.test.wav", "FOUNDRY_TEST_WAV"));
        Configuration config = Configuration.builder("java-audio-language-test")
                .runtimeDirectory(Path.of(required("foundry.test.runtime", "FOUNDRY_LOCAL_NATIVE_BIN_DIR")))
                .modelCacheDirectory(Path.of(required("foundry.test.cache", "FOUNDRY_TEST_DATA_DIR")))
                .appDataDirectory(temporary)
                .build();
        byte[] pcm = WavAudio.read(wav).pcm();
        assertTrue(pcm.length > 0, "Provide a nonempty speech fixture");
        try (FoundryLocalManager manager = new FoundryLocalManager(config)) {
            Model model = manager.catalog().getModelVariant(required("foundry.test.model", "FOUNDRY_TEST_MODEL"));
            assertTrue(model.isCached(), "Explicitly prepare the multilingual model first");
            System.err.println("language-test: runtime=" + manager.runtimeVersion() + ", model=" + model.info().id());
            model.load();
            try (AudioSession session = model.createAudioSession()) {
                for (String invalid : new String[] {"", " ", "en-US\0"}) {
                    assertThrows(IllegalArgumentException.class,
                            () -> session.streamPcm(PcmFormat.SPEECH, invalid, event -> {}));
                    assertThrows(IllegalArgumentException.class,
                            () -> session.transcribeWav(temporary.resolve("missing.wav"), invalid, event -> {}));
                }

                Transcript defaultPcm = pcm(session, pcm, null, true);
                Transcript defaultWav = wav(session, wav, null, true);
                if (Boolean.getBoolean("foundry.test.language.distinguishesDefault")) {
                    assertFalse(contains(defaultPcm.text(), expected), "PCM fixture must distinguish omission");
                    assertFalse(contains(defaultWav.text(), expected), "WAV fixture must distinguish omission");
                }
                for (String hint : new String[] {language, "auto"}) {
                    assertTranscript(pcm(session, pcm, hint, false), expected);
                    assertTranscript(wav(session, wav, hint, false), expected);
                    assertEquals(defaultPcm, pcm(session, pcm, null, false), "PCM hint leaked to the next request");
                    assertEquals(defaultWav, wav(session, wav, null, false), "WAV hint leaked to the next request");
                }
                assertEquals(defaultPcm, pcm(session, pcm, null, true), "Legacy PCM overload changed");
                assertEquals(defaultWav, wav(session, wav, null, true), "Legacy WAV overload changed");

                AtomicInteger callbacks = new AtomicInteger();
                Transcription cancelled;
                try (Transcription run = session.streamPcm(
                        PcmFormat.SPEECH, language, event -> callbacks.incrementAndGet())) {
                    cancelled = run;
                    assertThrows(IllegalStateException.class,
                            () -> session.streamPcm(PcmFormat.SPEECH, "auto", event -> {}));
                    assertThrows(IllegalStateException.class,
                            () -> session.transcribeWav(wav, "auto", event -> {}));
                    run.writePcm(Arrays.copyOf(pcm, Math.min(3200, pcm.length)));
                    run.cancel();
                    assertTrue(run.await(Duration.ofSeconds(30)).cancelled());
                }
                assertThrows(IllegalStateException.class, () -> cancelled.writePcm(new byte[2]));
                int count = callbacks.get();
                Thread.sleep(100);
                assertEquals(count, callbacks.get(), "Callbacks must not outlive close");
                assertTranscript(wav(session, wav, language, false), expected);
                Transcription closedRun = session.streamPcm(PcmFormat.SPEECH, "auto", event -> {});
                session.close();
                assertTrue(closedRun.isClosed());
                assertThrows(IllegalStateException.class,
                        () -> session.streamPcm(PcmFormat.SPEECH, language, event -> {}));
                assertThrows(IllegalStateException.class,
                        () -> session.transcribeWav(wav, language, event -> {}));
            } finally {
                model.unload();
            }
        }
    }

    private static Transcript pcm(AudioSession session, byte[] pcm, String language, boolean legacy) throws Exception {
        StringBuffer streamed = new StringBuffer();
        try (Transcription run = legacy
                ? session.streamPcm(PcmFormat.SPEECH, event -> streamed.append(event.text()))
                : session.streamPcm(PcmFormat.SPEECH, language, event -> streamed.append(event.text()))) {
            for (int offset = 0; offset < pcm.length; offset += 3200) {
                run.writePcm(Arrays.copyOfRange(pcm, offset, Math.min(offset + 3200, pcm.length)));
            }
            run.finishInput();
            return result(run, streamed, "PCM", language);
        }
    }

    private static Transcript wav(AudioSession session, Path wav, String language, boolean legacy) throws Exception {
        StringBuffer streamed = new StringBuffer();
        try (Transcription run = legacy
                ? session.transcribeWav(wav, event -> streamed.append(event.text()))
                : session.transcribeWav(wav, language, event -> streamed.append(event.text()))) {
            return result(run, streamed, "WAV", language);
        }
    }

    private static Transcript result(Transcription run, StringBuffer streamed, String input, String language)
            throws Exception {
        TranscriptionResult result = run.await(Duration.ofSeconds(60));
        assertFalse(result.cancelled());
        assertEquals(2, result.nativeFinishReason());
        Transcript transcript = new Transcript(streamed.toString(), result.text());
        System.err.println("language-test: " + input + " language=" + language + " " + transcript);
        return transcript;
    }

    private static void assertTranscript(Transcript transcript, String expected) {
        assertTrue(contains(transcript.streamed(), expected), () -> "Missing phrase in streaming: " + transcript);
        assertTrue(contains(transcript.text(), expected), () -> "Missing phrase in final: " + transcript);
    }

    private static boolean contains(String text, String expected) {
        return text.toLowerCase(Locale.ROOT).contains(expected.toLowerCase(Locale.ROOT));
    }

    private static String required(String property, String environmentVariable) {
        String value = setting(property, environmentVariable);
        assertNotNull(value, "Missing " + property);
        return value;
    }

    private static String setting(String property, String environmentVariable) {
        String value = System.getProperty(property, System.getenv(environmentVariable));
        return value == null || value.isBlank() ? null : value;
    }

    private record Transcript(String streamed, String text) {}
}
