/* Nordstjernen — pure-Java client of the renderer process protocol.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

package org.nordstjernen;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * Spawns and drives a {@code nordstjernen-renderer} child process over its
 * stdio control channel, speaking the same HTTP/JSON protocol the GTK shell
 * uses. This is a thin wrapper: all rendering happens in the separate
 * renderer process, so the JVM never loads the native engine.
 *
 * <p>Not thread-safe; drive one instance from a single thread.
 */
final class RendererProcess implements AutoCloseable {

    /** A parsed renderer response: status code, headers (lower-cased keys), body. */
    static final class Response {
        final int status;
        final Map<String, String> headers;
        final byte[] body;

        Response(int status, Map<String, String> headers, byte[] body) {
            this.status = status;
            this.headers = headers;
            this.body = body;
        }

        String header(String name) {
            return headers.get(name.toLowerCase(Locale.ROOT));
        }
    }

    private final Process process;
    private final OutputStream toRenderer;
    private final InputStream fromRenderer;

    RendererProcess(int maxWidth, int maxHeight) {
        this(maxWidth, maxHeight, false);
    }

    /**
     * @param privateMode launch the renderer in private/incognito mode, where
     *                    cookies, cache and storage are ephemeral
     */
    RendererProcess(int maxWidth, int maxHeight, boolean privateMode) {
        String exe = locateRenderer();
        List<String> argv = new ArrayList<>(List.of(
            exe, Integer.toString(maxWidth), Integer.toString(maxHeight), "stdio"));
        if (privateMode) {
            argv.add("private");
        }
        ProcessBuilder pb = new ProcessBuilder(argv);
        pb.redirectError(ProcessBuilder.Redirect.INHERIT);
        try {
            this.process = pb.start();
        } catch (IOException e) {
            throw new NordstjernenException("failed to start renderer: " + exe, e);
        }
        this.toRenderer = process.getOutputStream();
        this.fromRenderer = new BufferedInputStream(process.getInputStream());
    }

    private static String locateRenderer() {
        String configured = System.getProperty("nordstjernen.renderer",
            System.getenv("NORDSTJERNEN_RENDERER"));
        if (configured != null && !configured.isEmpty()) {
            return configured;
        }
        boolean win = System.getProperty("os.name", "")
            .toLowerCase(Locale.ROOT).contains("win");
        String name = win ? "nordstjernen-renderer.exe" : "nordstjernen-renderer";
        // Probe a few locations relative to the working directory.
        String[] candidates = {
            name,
            "builddir/src/" + name,
            "../builddir/src/" + name,
        };
        for (String c : candidates) {
            if (new File(c).isFile()) {
                return new File(c).getAbsolutePath();
            }
        }
        return name;
    }

    /** Send a request and read the full response (blocking). */
    synchronized Response request(String method, String path, String jsonBody) {
        byte[] body = jsonBody == null
            ? new byte[0] : jsonBody.getBytes(StandardCharsets.UTF_8);
        String head = method + " " + path + " HTTP/1.1\r\n"
            + "Content-Type: application/json\r\n"
            + "Content-Length: " + body.length + "\r\n\r\n";
        try {
            toRenderer.write(head.getBytes(StandardCharsets.US_ASCII));
            if (body.length > 0) {
                toRenderer.write(body);
            }
            toRenderer.flush();
            return readResponse();
        } catch (IOException e) {
            throw new NordstjernenException("renderer IPC failed on " + path, e);
        }
    }

    private Response readResponse() throws IOException {
        String statusLine = readLine();
        if (statusLine == null) {
            throw new NordstjernenException("renderer closed the connection");
        }
        int status = 0;
        String[] parts = statusLine.split(" ");
        if (parts.length >= 2) {
            try { status = Integer.parseInt(parts[1]); } catch (NumberFormatException ignored) { }
        }
        Map<String, String> headers = new HashMap<>();
        String line;
        while ((line = readLine()) != null && !line.isEmpty()) {
            int colon = line.indexOf(':');
            if (colon > 0) {
                headers.put(line.substring(0, colon).trim().toLowerCase(Locale.ROOT),
                            line.substring(colon + 1).trim());
            }
        }
        int len = 0;
        String cl = headers.get("content-length");
        if (cl != null) {
            try { len = Integer.parseInt(cl); } catch (NumberFormatException ignored) { }
        }
        byte[] body = new byte[len];
        int off = 0;
        while (off < len) {
            int n = fromRenderer.read(body, off, len - off);
            if (n < 0) {
                throw new NordstjernenException("renderer truncated the response body");
            }
            off += n;
        }
        return new Response(status, headers, body);
    }

    private String readLine() throws IOException {
        StringBuilder sb = new StringBuilder();
        int c;
        while ((c = fromRenderer.read()) != -1) {
            if (c == '\r') {
                int next = fromRenderer.read();
                if (next == '\n' || next == -1) {
                    return sb.toString();
                }
                sb.append('\r').append((char) next);
            } else if (c == '\n') {
                return sb.toString();
            } else {
                sb.append((char) c);
            }
        }
        return sb.length() == 0 ? null : sb.toString();
    }

    @Override
    public void close() {
        try {
            request("POST", "/quit", "");
        } catch (RuntimeException ignored) {
            // best effort
        }
        try { toRenderer.close(); } catch (IOException ignored) { }
        try { fromRenderer.close(); } catch (IOException ignored) { }
        process.destroy();
        try {
            process.waitFor();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }
}
