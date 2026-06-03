/* Nordstjernen — engine error.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

package org.nordstjernen;

/** Thrown when the native engine reports a failure. */
public class NordstjernenException extends RuntimeException {

    public NordstjernenException(String message) {
        super(message);
    }
}
