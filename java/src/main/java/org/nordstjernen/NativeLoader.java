/* Nordstjernen — native library loader.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

package org.nordstjernen;

import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;

/**
 * Loads {@code libnordstjernen} (the engine) and {@code libnordstjernenjni}
 * (this bridge). Resolution order:
 * <ol>
 *   <li>the directory in the {@code nordstjernen.native.dir} system property;</li>
 *   <li>libraries bundled in the jar under {@code /native/<os>-<arch>/}, extracted
 *       to a temp dir (the bridge's RPATH={@code $ORIGIN} finds the co-located engine);</li>
 *   <li>the platform loader ({@code java.library.path} / {@code LD_LIBRARY_PATH}).</li>
 * </ol>
 */
final class NativeLoader {

    private static boolean loaded = false;

    private NativeLoader() {
    }

    static synchronized void load() {
        if (loaded) {
            return;
        }
        String dir = System.getProperty("nordstjernen.native.dir");
        if (dir != null && !dir.isEmpty()) {
            loadFromDir(Path.of(dir));
        } else if (!loadFromResources()) {
            System.loadLibrary("nordstjernen");
            System.loadLibrary("nordstjernenjni");
        }
        loaded = true;
    }

    private static void loadFromDir(Path dir) {
        Path base = dir.toAbsolutePath().normalize();
        System.load(base.resolve(System.mapLibraryName("nordstjernen")).toString());
        System.load(base.resolve(System.mapLibraryName("nordstjernenjni")).toString());
    }

    private static boolean loadFromResources() {
        String base = "/native/" + platform() + "/";
        String engine = System.mapLibraryName("nordstjernen");
        String jni = System.mapLibraryName("nordstjernenjni");
        if (NativeLoader.class.getResource(base + jni) == null) {
            return false;
        }
        try {
            Path tmp = Files.createTempDirectory("nordstjernen-native");
            tmp.toFile().deleteOnExit();
            Path enginePath = extract(base + engine, tmp.resolve(engine));
            Path jniPath = extract(base + jni, tmp.resolve(jni));
            if (jniPath == null) {
                return false;
            }
            if (enginePath != null) {
                System.load(enginePath.toString());
            }
            System.load(jniPath.toString());
            return true;
        } catch (IOException e) {
            throw new NordstjernenException("failed to extract native libraries: " + e.getMessage());
        }
    }

    private static Path extract(String resource, Path target) throws IOException {
        try (InputStream in = NativeLoader.class.getResourceAsStream(resource)) {
            if (in == null) {
                return null;
            }
            Files.copy(in, target, StandardCopyOption.REPLACE_EXISTING);
            target.toFile().deleteOnExit();
            return target;
        }
    }

    private static String platform() {
        String os = System.getProperty("os.name", "").toLowerCase();
        String arch = System.getProperty("os.arch", "").toLowerCase();
        String o = os.contains("win") ? "windows"
                : (os.contains("mac") || os.contains("darwin")) ? "macos"
                : "linux";
        String a = switch (arch) {
            case "amd64", "x86_64" -> "x86_64";
            case "aarch64", "arm64" -> "aarch64";
            default -> arch;
        };
        return o + "-" + a;
    }
}
