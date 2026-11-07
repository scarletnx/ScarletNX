// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.scarletnx;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import java.io.File;
import java.io.IOException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.stream.Stream;
import org.junit.Test;

/** The calls between the native code and the app's Java, which only fail at run time, on the device: each method the
    C code looks up by name and JNI signature is there, and each native method has its C function, the same way round */
public class JniContractTest {
    private static final Class<?>[] CLASSES = {DataProvider.class, GameActivity.class, MainActivity.class, Makers.class,
                                               RecompileService.class};

    /** the repository's src/ (build.gradle passes it) */
    private static Path source() {
        String path = System.getProperty("scarletnx.source");
        assertTrue("build.gradle sets scarletnx.source", path != null);
        return new File(path).toPath();
    }

    private static String read(String file) throws IOException {
        return new String(Files.readAllBytes(source().resolve(file)), StandardCharsets.UTF_8);
    }

    private static String descriptor(Class<?> type) {
        if (type.isArray()) return "[" + descriptor(type.getComponentType());
        if (type == void.class) return "V";
        if (type == boolean.class) return "Z";
        if (type == byte.class) return "B";
        if (type == char.class) return "C";
        if (type == short.class) return "S";
        if (type == int.class) return "I";
        if (type == long.class) return "J";
        if (type == float.class) return "F";
        if (type == double.class) return "D";
        return "L" + type.getName().replace('.', '/') + ";";
    }

    private static String descriptor(Method method) {
        StringBuilder text = new StringBuilder("(");
        for (Class<?> parameter : method.getParameterTypes()) text.append(descriptor(parameter));
        return text.append(')').append(descriptor(method.getReturnType())).toString();
    }

    /** an instance method of type or its superclasses, as JNI's GetMethodID finds it */
    private static void assertInstanceMethod(Class<?> type, String name, String signature, String caller) {
        for (Class<?> at = type; at != null; at = at.getSuperclass())
            for (Method method : at.getDeclaredMethods())
                if (method.getName().equals(name) && descriptor(method).equals(signature)) {
                    assertFalse(caller + ": " + type.getSimpleName() + "." + name + " is static: GetMethodID misses it",
                                Modifier.isStatic(method.getModifiers()));
                    return;
                }
        fail(caller + " calls " + type.getSimpleName() + "." + name + signature + ", which it does not have");
    }

    /** src/gui/android.c's call(): the menu's activity, MainActivity */
    @Test
    public void everyMethodTheMenuCallsIsInMainActivity() throws IOException {
        Matcher call = Pattern.compile("call\\(\"(\\w+)\", \"([^\"]*)\"").matcher(read("gui/android.c"));
        int calls = 0;
        while (call.find()) {
            if (call.group(2).isEmpty()) continue; // no JNI environment to call with: it only says so
            assertInstanceMethod(MainActivity.class, call.group(1), call.group(2), "src/gui/android.c");
            ++calls;
        }
        assertTrue("src/gui/android.c calls MainActivity (" + calls + ")", calls >= 6);
    }

    /** src/gui/android.c's ask(): the menu's activity's methods that answer text */
    @Test
    public void everyMethodTheMenuAsksIsInMainActivity() throws IOException {
        Matcher ask = Pattern.compile("ask\\(\"(\\w+)\", \"([^\"]*)\"").matcher(read("gui/android.c"));
        int asks = 0;
        while (ask.find()) {
            assertTrue("src/gui/android.c: " + ask.group(1) + " answers a byte[]", ask.group(2).endsWith(")[B"));
            assertInstanceMethod(MainActivity.class, ask.group(1), ask.group(2), "src/gui/android.c");
            ++asks;
        }
        assertTrue("src/gui/android.c asks MainActivity (" + asks + ")", asks >= 1);
    }

    /** the runtime's Share Log and its start mark's reason, in a game's process: GameActivity */
    @Test
    public void theGamesShareLogIsInGameActivity() throws IOException {
        String sdl3 = read("nx/runtime/platform/backend/sdl3.c");
        for (String method : new String[] {"share", "exitReason"}) {
            Matcher at = Pattern.compile("GetMethodID\\(env, type, \"" + method + "\", \"([^\"]+)\"\\)").matcher(sdl3);
            assertTrue("sdl3.c looks " + method + " up", at.find());
            assertInstanceMethod(GameActivity.class, method, at.group(1), "src/nx/runtime/platform/backend/sdl3.c");
        }
    }

    private static final Map<String, String> JNI_TYPES = new TreeMap<>();
    static {
        JNI_TYPES.put("jboolean", "Z");
        JNI_TYPES.put("jbyte", "B");
        JNI_TYPES.put("jint", "I");
        JNI_TYPES.put("jlong", "J");
        JNI_TYPES.put("jbyteArray", "[B");
        JNI_TYPES.put("jstring", "Ljava/lang/String;");
        JNI_TYPES.put("jobjectArray", "[");
    }

    /** every Java_org_scarletnx_<class>_<method> the C code defines, its parameters after JNIEnv */
    private static Map<String, List<String>> nativeFunctions() throws IOException {
        Pattern function = Pattern.compile(
            "JNIEXPORT \\w+ JNICALL Java_org_scarletnx_(\\w+?)_(\\w+)\\(JNIEnv\\* \\w+((?:, \\w+ \\w+)*)\\)\\s*\\{");
        Map<String, List<String>> found = new TreeMap<>();
        List<Path> files = new ArrayList<>();
        try (Stream<Path> walk = Files.walk(source())) {
            walk.filter(path -> path.toString().endsWith(".c")).forEach(files::add);
        }
        for (Path file : files) {
            Matcher at = function.matcher(new String(Files.readAllBytes(file), StandardCharsets.UTF_8));
            while (at.find()) {
                List<String> parameters = new ArrayList<>();
                for (String parameter : at.group(3).split(", "))
                    if (!parameter.isEmpty()) parameters.add(parameter.split(" ")[0]);
                found.put(at.group(1) + "." + at.group(2), parameters);
            }
        }
        return found;
    }

    @Test
    public void everyNativeMethodHasItsCFunctionAndEveryCFunctionItsMethod() throws IOException {
        Map<String, List<String>> functions = nativeFunctions();
        int natives = 0;
        for (Class<?> type : CLASSES)
            for (Method method : type.getDeclaredMethods()) {
                if (!Modifier.isNative(method.getModifiers())) continue;
                String name = type.getSimpleName() + "." + method.getName();
                List<String> c = functions.remove(name);
                assertTrue(name + " has no Java_org_scarletnx_" + name.replace('.', '_') + " in src/", c != null);
                // a static method's C function takes its class, an instance's the object
                assertEquals(name, Modifier.isStatic(method.getModifiers()) ? "jclass" : "jobject", c.get(0));
                Class<?>[] parameters = method.getParameterTypes();
                assertEquals(name + "'s parameters", parameters.length, c.size() - 1);
                for (int i = 0; i < parameters.length; ++i) {
                    String jni = JNI_TYPES.get(c.get(i + 1));
                    assertTrue(name + ": " + c.get(i + 1), jni != null && descriptor(parameters[i]).startsWith(jni));
                }
                ++natives;
            }
        assertTrue("the app has native methods (" + natives + ")", natives >= 2);
        assertTrue("C functions with no Java method: " + functions.keySet(), functions.isEmpty());
    }
}
