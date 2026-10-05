// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;

/** Strict, bounded RIFF/WAVE reader; rejects unsupported formats instead of silently converting. */
public record WavAudio(PcmFormat format, byte[] pcm) {
    public WavAudio {
        java.util.Objects.requireNonNull(format);
        if (pcm == null || pcm.length == 0 || pcm.length % 2 != 0) {
            throw new IllegalArgumentException("WAV must contain complete PCM samples");
        }
        pcm = pcm.clone();
    }
    @Override public byte[] pcm() { return pcm.clone(); }
    public double durationSeconds() { return pcm.length / 32000.0; }

    public static WavAudio read(Path path) throws IOException {
        long size = Files.size(path);
        if (size < 44 || size > 64L * 1024 * 1024) throw new IOException("WAV size must be 44 bytes..64 MiB");
        try (var input = Files.newInputStream(path)) {
            byte[] bytes = input.readNBytes(64 * 1024 * 1024 + 1);
            if (bytes.length > 64 * 1024 * 1024) throw new IOException("WAV exceeds 64 MiB");
            return parse(bytes);
        }
    }

    static WavAudio parse(byte[] bytes) throws IOException {
        if (bytes.length < 44 || !tag(bytes, 0).equals("RIFF") || !tag(bytes, 8).equals("WAVE")) {
            throw new IOException("Expected a RIFF/WAVE file");
        }
        ByteBuffer buffer = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
        long end = Integer.toUnsignedLong(buffer.getInt(4)) + 8;
        if (end != bytes.length) throw new IOException("RIFF size does not match file length");
        PcmFormat format = null;
        byte[] pcm = null;
        for (long offset = 12; offset < end;) {
            if (offset + 8 > end) throw new IOException("Truncated WAV chunk");
            int at = (int) offset;
            long length = Integer.toUnsignedLong(buffer.getInt(at + 4));
            long next = offset + 8 + length + (length & 1);
            if (next > end) throw new IOException("WAV chunk extends beyond RIFF");
            switch (tag(bytes, at)) {
                case "fmt " -> {
                    if (format != null || length < 16 || buffer.getShort(at + 8) != 1) {
                        throw new IOException("Expected a single PCM fmt chunk");
                    }
                    try {
                        format = new PcmFormat(buffer.getInt(at + 12), buffer.getShort(at + 10),
                                buffer.getShort(at + 22));
                    } catch (IllegalArgumentException e) { throw new IOException(e.getMessage(), e); }
                    if (buffer.getInt(at + 16) != 32000 || buffer.getShort(at + 20) != 2) {
                        throw new IOException("Invalid WAV byte rate or block alignment");
                    }
                }
                case "data" -> {
                    if (pcm != null || length == 0 || length % 2 != 0) throw new IOException("Invalid WAV data chunk");
                    pcm = Arrays.copyOfRange(bytes, at + 8, (int) (offset + 8 + length));
                }
                default -> { /* Skip legal RIFF metadata chunks, including their odd-byte padding. */ }
            }
            offset = next;
        }
        if (format == null || pcm == null) throw new IOException("WAV requires fmt and data chunks");
        return new WavAudio(format, pcm);
    }

    private static String tag(byte[] bytes, int offset) {
        return new String(bytes, offset, 4, StandardCharsets.US_ASCII);
    }
}
