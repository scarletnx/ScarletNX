// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.os;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/** File helpers. */
public final class FileUtils {
    private FileUtils() {}

    public static long copy(InputStream in, OutputStream out) throws IOException { return in.transferTo(out); }

    public static long copy(InputStream in, OutputStream out, CancellationSignal signal, java.util.concurrent.Executor executor,
            ProgressListener listener) throws IOException {
        byte[] buffer = new byte[8192];
        long total = 0;
        int n;
        while ((n = in.read(buffer)) != -1) {
            if (signal != null) signal.throwIfCanceled();
            out.write(buffer, 0, n);
            total += n;
            if (listener != null && executor != null) {
                long progress = total;
                executor.execute(() -> listener.onProgress(progress));
            }
        }
        return total;
    }

    public static void closeQuietly(AutoCloseable closeable) {
        if (closeable == null) return;
        try { closeable.close(); } catch (RuntimeException e) { throw e; } catch (Exception e) {}
    }

    public interface ProgressListener { void onProgress(long progress); }
}
