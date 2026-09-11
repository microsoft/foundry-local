// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import static org.junit.jupiter.api.Assertions.*;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import org.junit.jupiter.api.Test;

class AudioValidationTest {
    @Test void pcmRejectsInvalidFormatsAndChunks() {
        assertThrows(IllegalArgumentException.class, () -> new PcmFormat(44100, 1, 16));
        assertThrows(IllegalArgumentException.class, () -> new PcmFormat(16000, 2, 16));
        assertThrows(IllegalArgumentException.class, () -> new PcmFormat(16000, 1, 32));
        for (int length : new int[] {0, 1, 3, 32001, 32002}) {
            assertThrows(IllegalArgumentException.class, () -> PcmFormat.SPEECH.validateChunk(new byte[length]));
        }
        PcmFormat.SPEECH.validateChunk(new byte[3200]);
        PcmFormat.SPEECH.validateChunk(new byte[32000]);
    }

    @Test void wavPreservesPcmAndDefensivelyCopies() throws Exception {
        byte[] wav = wav();
        WavAudio audio = WavAudio.parse(wav);
        assertEquals(PcmFormat.SPEECH, audio.format());
        assertEquals(4 / 32000.0, audio.durationSeconds());
        byte[] samples = audio.pcm();
        samples[0] = 99;
        assertEquals(0, audio.pcm()[0]);
    }

    @Test void wavRejectsTruncationInvalidSizesAndEncoding() {
        assertThrows(IOException.class, () -> WavAudio.parse(new byte[0]));
        byte[] badLength = wav();
        badLength[4] = 100;
        assertThrows(IOException.class, () -> WavAudio.parse(badLength));
        byte[] floatAudio = wav();
        floatAudio[20] = 3;
        assertThrows(IOException.class, () -> WavAudio.parse(floatAudio));
        byte[] badRate = wav();
        badRate[24] = 1;
        assertThrows(IOException.class, () -> WavAudio.parse(badRate));
        byte[] chunkOverrun = wav();
        chunkOverrun[40] = 100;
        assertThrows(IOException.class, () -> WavAudio.parse(chunkOverrun));
        byte[] badAlignment = wav();
        badAlignment[32] = 4;
        assertThrows(IOException.class, () -> WavAudio.parse(badAlignment));
    }

    @Test void configurationHasNoNativeSideEffects() {
        var path = java.nio.file.Path.of("missing");
        assertNotNull(new Configuration("unit-test", path, path, path));
        assertThrows(IllegalArgumentException.class, () -> new Configuration("", path, path, path));
        assertThrows(IllegalArgumentException.class, () -> new Configuration("a\0b", path, path, path));
    }

    private static byte[] wav() {
        ByteBuffer b = ByteBuffer.allocate(48).order(ByteOrder.LITTLE_ENDIAN);
        b.put(new byte[] {'R', 'I', 'F', 'F'}).putInt(40).put(new byte[] {'W', 'A', 'V', 'E'});
        b.put(new byte[] {'f', 'm', 't', ' '}).putInt(16).putShort((short) 1).putShort((short) 1);
        b.putInt(16000).putInt(32000).putShort((short) 2).putShort((short) 16);
        b.put(new byte[] {'d', 'a', 't', 'a'}).putInt(4).putInt(0);
        return b.array();
    }
}
