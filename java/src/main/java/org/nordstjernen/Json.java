/* Nordstjernen — minimal JSON reader/writer for the renderer control protocol.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

package org.nordstjernen;

/**
 * Just enough JSON for the flat objects the renderer speaks. Keys are located
 * by scanning for a quoted token immediately followed by {@code ':'}, so a key
 * name appearing inside a string value can never be mistaken for the key.
 */
final class Json {

    private Json() {
    }

    static String escape(String s) {
        if (s == null) {
            return "";
        }
        StringBuilder sb = new StringBuilder(s.length() + 8);
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"': sb.append("\\\""); break;
                case '\\': sb.append("\\\\"); break;
                case '\n': sb.append("\\n"); break;
                case '\r': sb.append("\\r"); break;
                case '\t': sb.append("\\t"); break;
                default:
                    if (c < 0x20) sb.append(String.format("\\u%04x", (int) c));
                    else sb.append(c);
            }
        }
        return sb.toString();
    }

    /** The string value of {@code key}, unescaped; {@code ""} when absent. */
    static String string(String json, String key) {
        int i = valueStart(json, key);
        if (i < 0) {
            return "";
        }
        i = skipSpace(json, i);
        if (i >= json.length() || json.charAt(i) != '"') {
            return "";
        }
        StringBuilder sb = new StringBuilder();
        for (int j = i + 1; j < json.length(); j++) {
            char c = json.charAt(j);
            if (c == '"') {
                break;
            }
            if (c != '\\' || j + 1 >= json.length()) {
                sb.append(c);
                continue;
            }
            char nx = json.charAt(++j);
            switch (nx) {
                case 'n': sb.append('\n'); break;
                case 'r': sb.append('\r'); break;
                case 't': sb.append('\t'); break;
                case 'b': sb.append('\b'); break;
                case 'f': sb.append('\f'); break;
                case 'u':
                    if (j + 4 < json.length()) {
                        try {
                            sb.append((char) Integer.parseInt(
                                json.substring(j + 1, j + 5), 16));
                        } catch (NumberFormatException ignored) {
                            // leave the malformed escape out
                        }
                        j += 4;
                    }
                    break;
                default: sb.append(nx);
            }
        }
        return sb.toString();
    }

    /** The string value of {@code key}, or {@code null} when absent or empty. */
    static String stringOrNull(String json, String key) {
        String s = string(json, key);
        return s.isEmpty() ? null : s;
    }

    /** The integer value of {@code key}, or {@code fallback} when absent. */
    static int integer(String json, String key, int fallback) {
        int i = valueStart(json, key);
        if (i < 0) {
            return fallback;
        }
        i = skipSpace(json, i);
        int start = i;
        if (i < json.length() && (json.charAt(i) == '-' || json.charAt(i) == '+')) {
            i++;
        }
        int digits = i;
        while (i < json.length() && Character.isDigit(json.charAt(i))) {
            i++;
        }
        if (i == digits) {
            return fallback;
        }
        try {
            return Integer.parseInt(json.substring(start, i));
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    /** True when {@code key} holds a non-zero integer. */
    static boolean flag(String json, String key) {
        return integer(json, key, 0) != 0;
    }

    private static int skipSpace(String json, int i) {
        while (i < json.length() && Character.isWhitespace(json.charAt(i))) {
            i++;
        }
        return i;
    }

    private static int valueStart(String json, String key) {
        if (json == null) {
            return -1;
        }
        int n = json.length();
        int i = 0;
        int tokenStart = -1;
        boolean inString = false;
        while (i < n) {
            char c = json.charAt(i);
            if (!inString) {
                if (c == '"') {
                    inString = true;
                    tokenStart = i;
                }
                i++;
                continue;
            }
            if (c == '\\') {
                i += 2;
                continue;
            }
            if (c != '"') {
                i++;
                continue;
            }
            inString = false;
            int j = skipSpace(json, i + 1);
            if (j < n && json.charAt(j) == ':'
                && matches(json, tokenStart + 1, i, key)) {
                return j + 1;
            }
            i++;
        }
        return -1;
    }

    private static boolean matches(String json, int from, int to, String key) {
        if (to - from != key.length()) {
            return false;
        }
        return json.regionMatches(from, key, 0, key.length());
    }
}
