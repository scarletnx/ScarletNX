// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.chromium.support_lib_glue;

import android.webkit.WebSettings;
import java.lang.reflect.Array;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * The WebView provider's side of AndroidX WebKit's support library, which every Android WebView has and which AndroidX
 * finds through WebView.getWebViewClassLoader. The WebView here has no engine, so the provider is as inert: it supports
 * only ALGORITHMIC_DARKENING, a setting of WebSettings, and gives each other call a default value (an inert handler
 * for an object, an empty array or collection, false, 0). The objects cross the boundary as InvocationHandlers that
 * AndroidX wraps in its boundary interfaces, called by method name, as Chromium's glue does.
 */
public final class SupportLibReflectionUtil {
    /** the features of the provider: only settings that WebSettings keeps */
    private static final String[] FEATURES = {"ALGORITHMIC_DARKENING"};

    private SupportLibReflectionUtil() {}

    public static InvocationHandler createWebViewProviderFactory() { return new Factory(); }

    /** An object that answers every call with a default value */
    static class Inert implements InvocationHandler {
        @Override public Object invoke(Object proxy, Method method, Object[] args) {
            switch (method.getName()) {
                case "equals": return args != null && args.length == 1 && proxy == args[0];
                case "hashCode": return System.identityHashCode(proxy);
                case "toString": return getClass().getSimpleName();
                default: return empty(method.getReturnType());
            }
        }
    }

    static Object empty(Class<?> type) {
        if (type == void.class) return null;
        if (type == boolean.class) return false;
        if (type == int.class) return 0;
        if (type == long.class) return 0L;
        if (type == float.class) return 0f;
        if (type == double.class) return 0d;
        if (type == short.class) return (short) 0;
        if (type == byte.class) return (byte) 0;
        if (type == char.class) return '\0';
        if (type.isArray()) return Array.newInstance(type.getComponentType(), 0);
        if (type == Set.class) return Set.of();
        if (type == List.class || type == java.util.Collection.class) return List.of();
        if (type == Map.class) return Map.of();
        if (type == InvocationHandler.class) return new Inert();
        return null;
    }

    /** WebViewProviderFactoryBoundaryInterface */
    static final class Factory extends Inert {
        @Override public Object invoke(Object proxy, Method method, Object[] args) {
            switch (method.getName()) {
                case "getSupportedFeatures": return FEATURES.clone();
                case "getWebkitToCompatConverter": return new Converter();
                default: return super.invoke(proxy, method, args);
            }
        }
    }

    /** WebkitToCompatConverterBoundaryInterface: a WebView's settings, the rest inert */
    static final class Converter extends Inert {
        @Override public Object invoke(Object proxy, Method method, Object[] args) {
            if (method.getName().equals("convertSettings") && args != null && args.length == 1 && args[0] instanceof WebSettings s)
                return new Settings(s);
            return super.invoke(proxy, method, args);
        }
    }

    /**
     * WebSettingsBoundaryInterface: a call goes to WebSettings' method of the same name and parameters when it has
     * one; the other settings are kept here and read back
     */
    static final class Settings extends Inert {
        private final WebSettings settings;
        private final Map<String, Object> values = new HashMap<>();

        Settings(WebSettings settings) { this.settings = settings; }

        @Override public Object invoke(Object proxy, Method method, Object[] args) {
            String name = method.getName();
            try {
                Method own = WebSettings.class.getMethod(name, method.getParameterTypes());
                return own.invoke(settings, args);
            } catch (NoSuchMethodException e) {
                // a setting WebSettings does not have
            } catch (ReflectiveOperationException e) {
                return empty(method.getReturnType());
            }
            int n = args != null ? args.length : 0;
            if (name.startsWith("set") && n == 1) {
                values.put(name.substring(3), args[0]);
                return null;
            }
            String key = name.startsWith("get") ? name.substring(3) : name.startsWith("is") ? name.substring(2) : null;
            if (key != null && n == 0 && values.containsKey(key)) return values.get(key);
            return super.invoke(proxy, method, args);
        }
    }
}
