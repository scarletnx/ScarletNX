// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package dalvik.system;

import java.io.File;
import java.io.IOException;
import java.util.Collections;
import java.util.Enumeration;

/**
 * A DEX file (deprecated since API 26). It keeps its path. The runtime translates only the APK's classes, so it lists
 * no entries and loads no class.
 */
@Deprecated
public final class DexFile {
    private final String fileName;

    public DexFile(File file) throws IOException { this(file.getPath()); }
    public DexFile(String fileName) throws IOException {
        if (!new File(fileName).isFile()) throw new IOException("No such file: " + fileName);
        this.fileName = fileName;
    }

    public String getName() { return fileName; }
    public Enumeration<String> entries() { return Collections.emptyEnumeration(); }
    public Class<?> loadClass(String name, ClassLoader loader) { return null; }
    public void close() throws IOException {}
    @Override public String toString() { return fileName; }
}
