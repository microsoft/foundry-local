// Copyright (c) Microsoft Corporation. Licensed under the MIT License.
package com.microsoft.foundry.local;

import java.net.URISyntaxException;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

final class NativeTestProcess {
    private NativeTestProcess() {}

    static Process start(Class<?> mainClass, String... args) throws java.io.IOException, URISyntaxException {
        String java = Path.of(System.getProperty("java.home"), "bin",
                System.getProperty("os.name").startsWith("Windows") ? "java.exe" : "java").toString();
        String classPath = String.join(System.getProperty("path.separator"),
                Path.of(NativeTestProcess.class.getProtectionDomain().getCodeSource().getLocation().toURI()).toString(),
                Path.of(FoundryLocalManager.class.getProtectionDomain().getCodeSource().getLocation().toURI()).toString(),
                Path.of(com.sun.jna.Native.class.getProtectionDomain().getCodeSource().getLocation().toURI()).toString());
        List<String> command = new ArrayList<>(List.of(java, "-cp", classPath, mainClass.getName()));
        command.addAll(List.of(args));
        return new ProcessBuilder(command).redirectErrorStream(true).start();
    }
}
