// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import static org.junit.jupiter.api.Assertions.*;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;

class TranscriptionStateTest {
    @Test void absentOrUnfinishedResponseIsNotCancellation() {
        assertThrows(IllegalStateException.class, () -> Transcription.cancelledResult(false, false, 0));
        assertThrows(IllegalStateException.class, () -> Transcription.cancelledResult(false, true, 0));
        assertThrows(IllegalStateException.class, () -> Transcription.cancelledResult(false, true, 1));
    }

    @Test void onlyAnObservedCancellationMakesACancelledResult() {
        assertTrue(Transcription.cancelledResult(true, false, 0));
        assertTrue(Transcription.cancelledResult(true, true, 0));
        assertFalse(Transcription.cancelledResult(false, true, 2));
    }

    @Test void cancellationDuringNativeResultDecodingWinsPublication() throws Exception {
        Transcription.Completion completion = new Transcription.Completion();
        CountDownLatch decoding = new CountDownLatch(1);
        CountDownLatch resume = new CountDownLatch(1);
        var worker = Executors.newSingleThreadExecutor();
        try {
            var published = worker.submit(() -> completion.complete(wasCancelled -> {
                assertFalse(wasCancelled);
                assertFalse(Thread.holdsLock(completion), "Native result decoding must not hold the state lock");
                decoding.countDown();
                try { assertTrue(resume.await(5, TimeUnit.SECONDS)); }
                catch (InterruptedException e) { throw new AssertionError(e); }
                return new TranscriptionResult("final transcript", "en", 6800L, false, 2, 100);
            }));
            assertTrue(decoding.await(5, TimeUnit.SECONDS));
            assertTrue(completion.cancel(), "Cancellation must be accepted before result publication");
            resume.countDown();
            published.get(5, TimeUnit.SECONDS);
            TranscriptionResult result = completion.result.get(5, TimeUnit.SECONDS);
            assertTrue(completion.isCancelled());
            assertTrue(result.cancelled(), "Publication must not use the pre-decoding cancellation snapshot");
            assertEquals("", result.text());
            assertEquals("", result.language());
            assertNull(result.durationMs());
            assertEquals(2, result.nativeFinishReason());
            assertFalse(completion.cancel(), "Further cancellation is too late after publication");
        } finally {
            resume.countDown();
            worker.shutdownNow();
            assertTrue(worker.awaitTermination(5, TimeUnit.SECONDS));
        }
    }

    @Test void cancellationAfterResultPublicationIsTooLate() {
        Transcription.Completion completion = new Transcription.Completion();
        completion.complete(wasCancelled -> new TranscriptionResult("final transcript", "en", 6800L, false, 2, 100));
        assertFalse(completion.cancel());
        assertFalse(completion.isCancelled());
        assertFalse(completion.result.join().cancelled());
        assertEquals("final transcript", completion.result.join().text());
    }

    @Test void cancellationAfterExceptionalPublicationIsTooLate() {
        Transcription.Completion completion = new Transcription.Completion();
        completion.fail(new IllegalStateException("native error"));
        assertFalse(completion.cancel());
        assertFalse(completion.isCancelled());
        assertTrue(completion.result.isCompletedExceptionally());
    }
}
