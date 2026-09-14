// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import com.sun.jna.Callback;
import com.sun.jna.Function;
import com.sun.jna.Memory;
import com.sun.jna.Native;
import com.sun.jna.NativeLibrary;
import com.sun.jna.Pointer;
import com.sun.jna.Structure;
import com.sun.jna.ptr.PointerByReference;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * Narrow binding to the stable API v1 prefix of the current Foundry Local C ABI.
 * All supported targets use 64-bit pointers/size_t. C bool is one byte, not JNA boolean.
 */
final class NativeApi {
    static final int VERSION = 1;
    static final ThreadLocal<Boolean> IN_CALLBACK = ThreadLocal.withInitial(() -> false);
    private static NativeApi resident;
    private final List<NativeLibrary> libraries = new ArrayList<>();
    final Path directory;
    final String version;
    final Table root, config, catalog, model, item, inference;

    static final class Root {
        static final int STATUS_RELEASE = 1;
        static final int MANAGER_CREATE = 4;
        static final int MANAGER_RELEASE = 5;
        static final int MANAGER_GET_CATALOG = 6;
        static final int GET_CATALOG_API = 10;
        static final int GET_CONFIGURATION_API = 11;
        static final int GET_ITEM_API = 12;
        static final int GET_INFERENCE_API = 13;
        static final int GET_MODEL_API = 14;
        static final int KEY_VALUE_PAIRS_CREATE = 15;
        static final int KEY_VALUE_PAIRS_ADD = 16;
        static final int KEY_VALUE_PAIRS_RELEASE = 20;
        static final int MODEL_LIST_RELEASE = 21;
        static final int MODEL_LIST_SIZE = 22;
        static final int MODEL_LIST_GET_AT = 23;
        static final int MANAGER_SHUTDOWN = 27;
    }

    static final class ConfigurationApi {
        static final int CREATE = 0;
        static final int RELEASE = 1;
        static final int SET_DEFAULT_LOG_LEVEL = 2;
        static final int SET_APP_DATA_DIRECTORY = 3;
        static final int SET_MODEL_CACHE_DIRECTORY = 5;
        static final int SET_ADDITIONAL_OPTIONS = 10;
    }

    static final class CatalogApi {
        static final int GET_MODELS = 1;
        static final int GET_MODEL_VARIANT = 3;
    }

    static final class ModelApi {
        static final int GET_INFO = 0;
        static final int IS_CACHED = 2;
        static final int GET_PATH = 3;
        static final int DOWNLOAD = 4;
        static final int IS_LOADED = 5;
        static final int LOAD = 6;
        static final int UNLOAD = 7;
        static final int GET_VARIANTS = 9;
        static final int INFO_GET_ID = 11;
        static final int INFO_GET_NAME = 12;
        static final int INFO_GET_VERSION = 13;
        static final int INFO_GET_ALIAS = 14;
        static final int INFO_GET_URI = 15;
        static final int INFO_GET_EXECUTION_PROVIDER = 17;
        static final int INFO_GET_TASK = 18;
        static final int INFO_GET_STRING_PROPERTY = 21;
    }

    static final class ItemApi {
        static final int CREATE = 0;
        static final int RELEASE = 1;
        static final int GET_TYPE = 2;
        static final int SET_BYTES = 3;
        static final int SET_AUDIO = 8;
        static final int GET_SPEECH_SEGMENT = 19;
        static final int GET_SPEECH_RESULT = 20;
        static final int GET_QUEUE = 23;
        static final int QUEUE_PUSH = 26;
        static final int QUEUE_TRY_POP = 27;
        static final int QUEUE_MARK_FINISHED = 29;
    }

    static final class InferenceApi {
        static final int REQUEST_CREATE = 0;
        static final int REQUEST_RELEASE = 1;
        static final int REQUEST_ADD_ITEM = 2;
        static final int REQUEST_CANCEL = 6;
        static final int RESPONSE_RELEASE = 8;
        static final int RESPONSE_GET_ITEM_COUNT = 9;
        static final int RESPONSE_GET_ITEM = 10;
        static final int RESPONSE_GET_FINISH_REASON = 11;
        static final int SESSION_CREATE = 13;
        static final int SESSION_RELEASE = 14;
        static final int SESSION_SET_STREAMING_CALLBACK = 15;
        static final int SESSION_PROCESS_REQUEST = 17;
    }

    static void outsideCallback() {
        if (IN_CALLBACK.get()) {
            throw new IllegalStateException("SDK lifecycle/input calls are not allowed from a native callback");
        }
    }

    static Throwable preserveFailure(Throwable first, Throwable next) {
        if (first == null) return next;
        if (first != next) first.addSuppressed(next);
        return first;
    }

    static void rethrow(Throwable failure) {
        if (failure == null) return;
        if (failure instanceof RuntimeException runtime) throw runtime;
        if (failure instanceof Error error) throw error;
        throw new AssertionError(failure);
    }

    static String target() {
        String arch = System.getProperty("os.arch").toLowerCase(Locale.ROOT);
        String cpu = switch (arch) {
            case "amd64", "x86_64" -> "x64";
            case "aarch64", "arm64" -> "arm64";
            default -> arch;
        };
        String os = System.getProperty("os.name").toLowerCase(Locale.ROOT);
        if (os.contains("win")) return "win-" + cpu;
        if (os.contains("linux")) return "linux-" + cpu;
        if (os.contains("mac")) return "osx-" + cpu;
        return os.replaceAll("[^a-z0-9]+", "-") + "-" + cpu;
    }

    static String foundryLibraryName() {
        String os = System.getProperty("os.name").toLowerCase(Locale.ROOT);
        if (os.contains("win")) return "foundry_local.dll";
        if (os.contains("mac")) return "libfoundry_local.dylib";
        return "libfoundry_local.so";
    }

    static Path findFoundryLibrary(Path directory) {
        Path library = directory.resolve(foundryLibraryName());
        if (!Files.isRegularFile(library)) {
            throw new IllegalArgumentException(
                    "Foundry Local native library is missing from " + directory + ": " + library.getFileName());
        }
        return library;
    }

    static synchronized NativeApi load(Path path) {
        outsideCallback();
        try {
            Path real = path.toRealPath();
            Path library = findFoundryLibrary(real);
            if (resident != null) {
                if (!resident.directory.equals(real)) {
                    throw new IllegalStateException("One native runtime directory per JVM is supported");
                }
                return resident;
            }
            resident = new NativeApi(real, library);
            return resident;
        } catch (IOException e) {
            throw new IllegalArgumentException("Cannot read native runtime directory: " + path, e);
        }
    }

    private static void verifyArchitecture() {
        if (Native.POINTER_SIZE != 8 || Native.SIZE_T_SIZE != 8) {
            throw new IllegalStateException("Only 64-bit JVMs are supported");
        }
    }

    private NativeApi(Path path, Path foundryLibrary) {
        verifyArchitecture();
        directory = path;
        for (String dependency : dependencyLibraryNames()) {
            Path candidate = path.resolve(dependency);
            if (Files.isRegularFile(candidate)) {
                libraries.add(open(candidate));
            }
        }
        NativeLibrary library = open(foundryLibrary);
        libraries.add(library);
        version = text(library.getFunction("FoundryLocalGetVersionString").invokePointer(new Object[0]));
        Pointer api = library.getFunction("FoundryLocalGetApi").invokePointer(new Object[] {VERSION});
        if (api == null) throw new IllegalStateException("Native runtime does not expose C API " + VERSION);
        root = new Table(api);
        catalog = new Table(root.pointer(Root.GET_CATALOG_API));
        config = new Table(root.pointer(Root.GET_CONFIGURATION_API));
        item = new Table(root.pointer(Root.GET_ITEM_API));
        inference = new Table(root.pointer(Root.GET_INFERENCE_API));
        model = new Table(root.pointer(Root.GET_MODEL_API));
    }

    private static List<String> dependencyLibraryNames() {
        String os = System.getProperty("os.name").toLowerCase(Locale.ROOT);
        if (os.contains("win")) {
            return List.of("onnxruntime.dll", "onnxruntime-genai.dll");
        }
        if (os.contains("mac")) {
            return List.of("libonnxruntime.1.dylib", "libonnxruntime.dylib", "libonnxruntime-genai.dylib");
        }
        return List.of("libonnxruntime.so.1", "libonnxruntime.so", "libonnxruntime-genai.so");
    }

    private static NativeLibrary open(Path file) {
        try {
            return NativeLibrary.getInstance(file.toString(), Map.of(
                    com.sun.jna.Library.OPTION_STRING_ENCODING, "UTF-8"));
        } catch (UnsatisfiedLinkError e) {
            throw new IllegalStateException("Cannot load " + file.getFileName()
                    + "; use a matching 64-bit JVM and install platform loader prerequisites", e);
        }
    }

    static String text(Pointer pointer) { return pointer == null ? "" : pointer.getString(0, "UTF-8"); }
    static Memory utf8(String value) {
        if (value.indexOf('\0') >= 0) throw new IllegalArgumentException("Strings must not contain NUL");
        byte[] bytes = value.getBytes(java.nio.charset.StandardCharsets.UTF_8);
        Memory memory = new Memory(bytes.length + 1L);
        memory.write(0, bytes, 0, bytes.length);
        memory.setByte(bytes.length, (byte) 0);
        return memory;
    }

    void check(Pointer status) {
        if (status == null) return;
        try {
            throw new FoundryLocalException(root.integer(2, status), text(root.pointer(3, status)));
        } finally {
            root.call(Root.STATUS_RELEASE, status);
        }
    }

    Pointer output(Table table, int slot, Object... args) {
        PointerByReference output = new PointerByReference();
        Object[] all = java.util.Arrays.copyOf(args, args.length + 1);
        all[args.length] = output;
        check(table.pointer(slot, all));
        return output.getValue();
    }

    Pointer create(Table table, int slot, Object... args) {
        Pointer output = output(table, slot, args);
        if (output == null) throw new IllegalStateException("Native API returned a null handle");
        return output;
    }

    static final class Table {
        private final Pointer table;
        Table(Pointer table) {
            if (table == null) throw new IllegalStateException("Missing native function table");
            this.table = table;
        }
        private Function function(int index) {
            Pointer function = table.getPointer(index * 8L);
            if (function == null) throw new IllegalStateException("Missing native function at slot " + index);
            return Function.getFunction(function, Function.C_CONVENTION, "UTF-8");
        }
        Pointer pointer(int index, Object... args) { return function(index).invokePointer(args); }
        int integer(int index, Object... args) { return function(index).invokeInt(args); }
        long size(int index, Object... args) { return function(index).invokeLong(args); }
        boolean bool(int index, Object... args) {
            return ((Byte) function(index).invoke(Byte.class, args)) != 0;
        }
        void call(int index, Object... args) { function(index).invokeVoid(args); }
    }

    interface ProgressCallback extends Callback { int invoke(float value, Pointer userData); }
    interface StreamCallback extends Callback { int invoke(CallbackData data, Pointer userData); }
    interface BytesDeleter extends Callback { void invoke(Pointer data, Pointer userData); }

    @Structure.FieldOrder({"version", "queue"})
    public static class CallbackData extends Structure implements Structure.ByValue {
        public int version;
        public Pointer queue;
    }

    @Structure.FieldOrder({"version", "data", "mutableData", "dataSize", "format", "uri",
            "sampleRate", "channels", "deleter", "userData"})
    public static class AudioData extends Structure {
        public int version = VERSION;
        public Pointer data, mutableData;
        public long dataSize;
        public Pointer format, uri;
        public int sampleRate, channels;
        public Pointer deleter, userData;
    }

    @Structure.FieldOrder({"version", "itemType", "data", "mutableData", "dataSize", "deleter", "userData"})
    public static class BytesData extends Structure {
        public int version = VERSION, itemType = 1;
        public Pointer data, mutableData;
        public long dataSize;
        public BytesDeleter deleter;
        public Pointer userData;
    }

    @Structure.FieldOrder({"version", "kind", "text", "start", "end", "utteranceStart",
            "words", "wordCount", "language"})
    public static class SegmentData extends Structure {
        public int version = VERSION, kind;
        public Pointer text;
        public long start, end;
        public byte utteranceStart;
        public Pointer words;
        public long wordCount;
        public Pointer language;
    }

    @Structure.FieldOrder({"version", "text", "language", "duration", "segments", "segmentCount"})
    public static class ResultData extends Structure {
        public int version = VERSION;
        public Pointer text, language;
        public long duration;
        public Pointer segments;
        public long segmentCount;
    }
}
