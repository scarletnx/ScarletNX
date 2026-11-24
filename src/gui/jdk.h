/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The JDKs of this machine, for Settings > Launch > Java home: the folders where each system's installers and package
   managers put them. No SDL */
#ifndef SNX_GUI_JDK_H
#define SNX_GUI_JDK_H
#include <stddef.h>

enum { GUI_JDK_LEAST = 21 }; /* the oldest the Android programs load (src/android/CMakeLists.txt) */
/* home's Java version, its release file's JAVA_VERSION ("21.0.4": 21, "1.8.0_402": 8), where it holds a JVM to load
   (lib/server's, bin/server's on Windows); 0 where it is no such folder */
int gui_jdk_version(const char* home);
/* the JDKs of GUI_JDK_LEAST or later in the folders of roots (a NULL ends them), by name: each folder of a root, its
   Contents/Home (macOS), and the folder itself; each once, the links of a folder (Debian's java-21-openjdk-amd64 beside
   java-1.21.0-openjdk-amd64) by the first that says it. Owned: *count of them, gui_jdks_free's */
char** gui_jdks(const char* const* roots, size_t* count);
/* the roots of this system, with home's own (SDKMAN's, IntelliJ's ~/.jdks, ASDF's and mise's); owned as gui_jdks' */
char** gui_jdk_roots(const char* home, size_t* count);
void gui_jdks_free(char** jdks, size_t count);
#endif
