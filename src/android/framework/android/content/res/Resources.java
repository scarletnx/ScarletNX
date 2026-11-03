// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.content.res;

import android.util.AttributeSet;
import android.util.DisplayMetrics;
import android.util.TypedValue;
import java.io.ByteArrayInputStream;
import java.io.InputStream;
import snx.android.Apk;

/** The app's resources for the device's configuration, over its AssetManager. */
public class Resources {
    public static final int ID_NULL = 0;
    private static Resources system;
    private static final java.lang.ref.Cleaner CLEANER = java.lang.ref.Cleaner.create();
    private final AssetManager assets;
    private final DisplayMetrics metrics = new DisplayMetrics();
    private final Configuration configuration = new Configuration();
    private ClassLoader classLoader;

    /** The framework's own resources: ScarletNX has none of its own, so the app's tables serve. */
    public static Resources getSystem() {
        synchronized (Resources.class) {
            if (system == null) system = new Resources(AssetManager.forApk(), snx.android.Device.metrics(), snx.android.Device.configuration());
            return system;
        }
    }

    public Resources(AssetManager assets, DisplayMetrics metrics, Configuration config) {
        this.assets = assets;
        updateConfiguration(config, metrics);
        synchronized (ALL) { ALL.put(this, Boolean.TRUE); }
    }

    /** Every Resources, which a change of the display updates, as Android's ResourcesManager updates its own */
    private static final java.util.WeakHashMap<Resources, Boolean> ALL = new java.util.WeakHashMap<>();

    /**
     * The display changed (snx.android.Device): each Resources takes its metrics, and the fields of the device's
     * configuration that follow the display's size. A configuration context keeps what it overrides otherwise.
     */
    public static void deviceChanged(Configuration device, DisplayMetrics m) {
        Resources[] all;
        synchronized (ALL) { all = ALL.keySet().toArray(new Resources[0]); }
        for (Resources r : all) {
            synchronized (r) {
                Configuration c = new Configuration(r.configuration);
                c.orientation = device.orientation;
                c.screenWidthDp = device.screenWidthDp;
                c.screenHeightDp = device.screenHeightDp;
                c.smallestScreenWidthDp = device.smallestScreenWidthDp;
                c.screenLayout = device.screenLayout;
                DisplayMetrics dm = new DisplayMetrics();
                dm.setTo(r.metrics);
                dm.widthPixels = m.widthPixels;
                dm.heightPixels = m.heightPixels;
                r.updateConfiguration(c, dm);
            }
        }
    }

    public Resources(ClassLoader classLoader) {
        this(AssetManager.forApk(), snx.android.Device.metrics(), snx.android.Device.configuration());
        this.classLoader = classLoader;
    }

    public final AssetManager getAssets() { return assets; }
    public ClassLoader getClassLoader() { return classLoader != null ? classLoader : getClass().getClassLoader(); }
    public Configuration getConfiguration() { return configuration; }
    public DisplayMetrics getDisplayMetrics() { return metrics; }

    @Deprecated
    public void updateConfiguration(Configuration config, DisplayMetrics m) {
        synchronized (this) {
            if (config != null) configuration.setTo(config);
            if (m != null) metrics.setTo(m);
            assets.setConfiguration(configuration, metrics);
        }
    }

    public static class NotFoundException extends RuntimeException {
        public NotFoundException() {}
        public NotFoundException(String name) { super(name); }
        public NotFoundException(String name, Exception cause) { super(name, cause); }
    }

    private static NotFoundException notFound(int id) {
        return new NotFoundException("Resource ID #0x" + Integer.toHexString(id));
    }

    public void getValue(int id, TypedValue outValue, boolean resolveRefs) throws NotFoundException {
        if (!assets.getResourceValue(id, outValue, resolveRefs)) throw notFound(id);
    }

    public void getValue(String name, TypedValue outValue, boolean resolveRefs) throws NotFoundException {
        int id = getIdentifier(name, "string", null);
        if (id == 0) throw new NotFoundException("String resource name " + name);
        getValue(id, outValue, resolveRefs);
    }

    public void getValueForDensity(int id, int density, TypedValue outValue, boolean resolveRefs) {
        getValue(id, outValue, resolveRefs);
    }

    TypedValue value(int id) {
        TypedValue v = new TypedValue();
        getValue(id, v, true);
        return v;
    }

    public CharSequence getText(int id) throws NotFoundException {
        TypedValue v = value(id);
        if (v.type == TypedValue.TYPE_STRING && v.string != null) return v.string;
        throw new NotFoundException("String resource ID #0x" + Integer.toHexString(id));
    }

    public CharSequence getText(int id, CharSequence def) {
        TypedValue v = new TypedValue();
        return assets.getResourceValue(id, v, true) && v.type == TypedValue.TYPE_STRING ? v.string : def;
    }

    public String getString(int id) throws NotFoundException {
        return getText(id).toString();
    }

    public String getString(int id, Object... formatArgs) throws NotFoundException {
        return String.format(configuration.getLocales().get(0) != null ? configuration.getLocales().get(0) : java.util.Locale.getDefault(),
            getString(id), formatArgs);
    }

    /** A plural's text for quantity: the bag's entry for its CLDR category (other when the bag has none for it). */
    public CharSequence getQuantityText(int id, int quantity) throws NotFoundException {
        int[] bag = bag(id);
        String[] keys = {"zero", "one", "two", "few", "many", "other"};
        int want = quantity == 0 ? 0 : quantity == 1 ? 1 : 5;
        // the attribute IDs of the quantities (^attr-private/quantity): 0x01000005 others, then zero to many
        int[] ids = {0x01000006, 0x01000007, 0x01000008, 0x01000009, 0x0100000a, 0x01000005};
        CharSequence other = null;
        for (int i = 0; i < bag.length; i += 8) {
            CharSequence text = bagString(bag, i);
            if (bag[i] == ids[want]) return text;
            if (bag[i] == ids[5]) other = text;
        }
        if (other != null) return other;
        throw new NotFoundException("Plural resource ID #0x" + Integer.toHexString(id) + " quantity=" + quantity + " (" + keys[want] + ")");
    }

    public String getQuantityString(int id, int quantity) { return getQuantityText(id, quantity).toString(); }

    public String getQuantityString(int id, int quantity, Object... formatArgs) {
        return String.format(getQuantityString(id, quantity), formatArgs);
    }

    int[] bag(int id) {
        int[] bag = assets.getBag(id);
        if (bag == null) throw notFound(id);
        return bag;
    }

    private CharSequence bagString(int[] bag, int i) {
        return bag[i + 1] == TypedValue.TYPE_STRING ? assets.getPooledString(bag[i + 3], bag[i + 2]) : null;
    }

    public CharSequence[] getTextArray(int id) throws NotFoundException {
        int[] bag = bag(id);
        CharSequence[] out = new CharSequence[bag.length / 8];
        for (int i = 0; i < out.length; ++i) out[i] = bagString(bag, i * 8);
        return out;
    }

    public String[] getStringArray(int id) throws NotFoundException {
        int[] bag = bag(id);
        String[] out = new String[bag.length / 8];
        for (int i = 0; i < out.length; ++i) {
            CharSequence s = bagString(bag, i * 8);
            out[i] = s != null ? s.toString() : null;
        }
        return out;
    }

    public int[] getIntArray(int id) throws NotFoundException {
        int[] bag = bag(id);
        int[] out = new int[bag.length / 8];
        for (int i = 0; i < out.length; ++i) out[i] = bag[i * 8 + 2];
        return out;
    }

    public TypedArray obtainTypedArray(int id) throws NotFoundException {
        int[] bag = bag(id);
        int n = bag.length / 8;
        TypedArray a = TypedArray.obtain(this, n);
        for (int i = 0; i < n; ++i) {
            System.arraycopy(bag, i * 8 + 1, a.data, i * TypedArray.STYLE_NUM_ENTRIES, TypedArray.STYLE_NUM_ENTRIES);
            if (a.data[i * TypedArray.STYLE_NUM_ENTRIES] != TypedValue.TYPE_NULL) a.indices[++a.indices[0]] = i;
        }
        return a;
    }

    public int getInteger(int id) throws NotFoundException {
        TypedValue v = value(id);
        if (v.type >= TypedValue.TYPE_FIRST_INT && v.type <= TypedValue.TYPE_LAST_INT) return v.data;
        throw new NotFoundException("Resource ID #0x" + Integer.toHexString(id) + " type #0x" + Integer.toHexString(v.type) + " is not valid");
    }

    public boolean getBoolean(int id) throws NotFoundException {
        return getInteger(id) != 0;
    }

    public float getFloat(int id) {
        TypedValue v = value(id);
        if (v.type == TypedValue.TYPE_FLOAT) return v.getFloat();
        throw new NotFoundException("Resource ID #0x" + Integer.toHexString(id) + " is not a float");
    }

    public float getFraction(int id, int base, int pbase) {
        return TypedValue.complexToFraction(value(id).data, base, pbase);
    }

    public float getDimension(int id) throws NotFoundException {
        TypedValue v = value(id);
        if (v.type == TypedValue.TYPE_DIMENSION) return TypedValue.complexToDimension(v.data, metrics);
        throw new NotFoundException("Resource ID #0x" + Integer.toHexString(id) + " is not a dimension");
    }

    public int getDimensionPixelOffset(int id) throws NotFoundException {
        TypedValue v = value(id);
        if (v.type == TypedValue.TYPE_DIMENSION) return TypedValue.complexToDimensionPixelOffset(v.data, metrics);
        throw new NotFoundException("Resource ID #0x" + Integer.toHexString(id) + " is not a dimension");
    }

    public int getDimensionPixelSize(int id) throws NotFoundException {
        TypedValue v = value(id);
        if (v.type == TypedValue.TYPE_DIMENSION) return TypedValue.complexToDimensionPixelSize(v.data, metrics);
        throw new NotFoundException("Resource ID #0x" + Integer.toHexString(id) + " is not a dimension");
    }

    @Deprecated
    public int getColor(int id) throws NotFoundException {
        return getColor(id, null);
    }

    public int getColor(int id, Theme theme) throws NotFoundException {
        TypedValue v = value(id);
        if (v.type >= TypedValue.TYPE_FIRST_INT && v.type <= TypedValue.TYPE_LAST_INT) return v.data;
        if (v.type == TypedValue.TYPE_STRING) return loadColorStateList(v, id, theme).getDefaultColor();
        throw new NotFoundException("Resource ID #0x" + Integer.toHexString(id) + " type #0x" + Integer.toHexString(v.type) + " is not valid");
    }

    @Deprecated
    public ColorStateList getColorStateList(int id) throws NotFoundException {
        return getColorStateList(id, null);
    }

    public ColorStateList getColorStateList(int id, Theme theme) throws NotFoundException {
        TypedValue v = value(id);
        return loadColorStateList(v, id, theme);
    }

    ColorStateList loadColorStateList(TypedValue v, int id, Theme theme) {
        if (v.type >= TypedValue.TYPE_FIRST_COLOR_INT && v.type <= TypedValue.TYPE_LAST_COLOR_INT)
            return ColorStateList.valueOf(v.data);
        if (v.type == TypedValue.TYPE_STRING && v.string != null && v.string.toString().endsWith(".xml")) {
            try (XmlResourceParser p = getXmlFile(v.string.toString())) {
                return ColorStateList.createFromXml(this, p, theme);
            } catch (Exception e) {
                throw new NotFoundException("File " + v.string + " from color state list resource ID #0x" + Integer.toHexString(id), e);
            }
        }
        throw notFound(id);
    }

    public android.graphics.drawable.Drawable getDrawable(int id) throws NotFoundException {
        return getDrawable(id, null);
    }

    public android.graphics.drawable.Drawable getDrawable(int id, Theme theme) throws NotFoundException {
        return android.graphics.drawable.Drawable.load(this, id, theme);
    }

    public android.graphics.drawable.Drawable getDrawableForDensity(int id, int density, Theme theme) {
        return getDrawable(id, theme);
    }

    public android.graphics.Typeface getFont(int id) throws NotFoundException {
        return android.graphics.Typeface.DEFAULT;
    }

    XmlResourceParser getXmlFile(String path) {
        byte[] data = Apk.read(path);
        if (data == null) throw new NotFoundException("File " + path);
        return new XmlBlock(data).newParser();
    }

    public XmlResourceParser getXml(int id) throws NotFoundException {
        return loadXml(id, "xml");
    }

    public XmlResourceParser getLayout(int id) throws NotFoundException {
        return loadXml(id, "layout");
    }

    public XmlResourceParser getAnimation(int id) throws NotFoundException {
        return loadXml(id, "anim");
    }

    XmlResourceParser loadXml(int id, String kind) {
        TypedValue v = value(id);
        if (v.type == TypedValue.TYPE_STRING && v.string != null) return getXmlFile(v.string.toString());
        throw new NotFoundException("Resource ID #0x" + Integer.toHexString(id) + " (" + kind + ") is not an XML file");
    }

    public InputStream openRawResource(int id) throws NotFoundException {
        TypedValue v = value(id);
        byte[] data = v.type == TypedValue.TYPE_STRING && v.string != null ? Apk.read(v.string.toString()) : null;
        if (data == null) throw notFound(id);
        return new ByteArrayInputStream(data);
    }

    public InputStream openRawResource(int id, TypedValue value) throws NotFoundException {
        getValue(id, value, true);
        return openRawResource(id);
    }

    public AssetFileDescriptor openRawResourceFd(int id) throws NotFoundException {
        throw new NotFoundException("snx: openRawResourceFd of a resource");
    }

    public int getIdentifier(String name, String defType, String defPackage) {
        if (name == null) return 0;
        try {
            return Integer.parseInt(name);
        } catch (NumberFormatException ignored) {
            // a name
        }
        String pkg = defPackage, type = defType, entry = name;
        if (entry.startsWith("@")) entry = entry.substring(1);
        int colon = entry.indexOf(':');
        if (colon >= 0) {
            pkg = entry.substring(0, colon);
            entry = entry.substring(colon + 1);
        }
        int slash = entry.indexOf('/');
        if (slash >= 0) {
            type = entry.substring(0, slash);
            entry = entry.substring(slash + 1);
        }
        return type == null ? 0 : assets.getResourceIdentifier(entry, type, pkg);
    }

    public String getResourceName(int id) throws NotFoundException {
        String name = assets.getResourceName(id);
        if (name == null) throw new NotFoundException("Unable to find resource ID #0x" + Integer.toHexString(id));
        return name;
    }

    public String getResourcePackageName(int id) throws NotFoundException {
        String n = getResourceName(id);
        return n.substring(0, n.indexOf(':'));
    }

    public String getResourceTypeName(int id) throws NotFoundException {
        String n = getResourceName(id);
        return n.substring(n.indexOf(':') + 1, n.indexOf('/'));
    }

    public String getResourceEntryName(int id) throws NotFoundException {
        String n = getResourceName(id);
        return n.substring(n.indexOf('/') + 1);
    }

    public final Theme newTheme() {
        return new Theme(this);
    }

    public TypedArray obtainAttributes(AttributeSet set, int[] attrs) {
        TypedArray a = TypedArray.obtain(this, attrs.length);
        XmlBlock.Parser parser = set instanceof XmlBlock.Parser p ? p : null;
        assets.retrieveAttributes(parser, attrs, a.data, a.indices);
        a.xml = parser;
        return a;
    }

    public void flushLayoutCache() {}

    /** A theme: the styles applied, the attributes resolved by the HLE of libandroidfw's Theme. */
    public final class Theme {
        private final long handle;

        Theme(Resources owner) {
            handle = assets.createTheme();
            CLEANER.register(this, new Release(assets, handle));
        }

        private record Release(AssetManager assets, long handle) implements Runnable {
            @Override public void run() { assets.destroyTheme(handle); }
        }

        long handle() { return handle; }

        public void applyStyle(int resId, boolean force) {
            assets.applyStyle(handle, resId, force);
        }

        public void setTo(Theme other) {
            assets.copyTheme(handle, other.handle);
        }

        public Resources getResources() {
            return Resources.this;
        }

        public void rebase() {
            int[] styles = assets.getThemeStyles(handle);
            assets.clearTheme(handle);
            for (int i = 0; styles != null && i < styles.length; i += 2) assets.applyStyle(handle, styles[i], styles[i + 1] != 0);
        }

        public void rebase(Resources.Theme other) {
            setTo(other);
        }

        public boolean resolveAttribute(int resid, TypedValue outValue, boolean resolveRefs) {
            return assets.getThemeValue(handle, resid, outValue, resolveRefs);
        }

        public TypedArray obtainStyledAttributes(int[] attrs) {
            return obtainStyledAttributes(null, attrs, 0, 0);
        }

        public TypedArray obtainStyledAttributes(int resId, int[] attrs) throws NotFoundException {
            return obtainStyledAttributes(null, attrs, 0, resId);
        }

        public TypedArray obtainStyledAttributes(AttributeSet set, int[] attrs, int defStyleAttr, int defStyleRes) {
            TypedArray a = TypedArray.obtain(Resources.this, attrs.length);
            XmlBlock.Parser parser = set instanceof XmlBlock.Parser p ? p : null;
            assets.applyStyle(handle, parser, defStyleAttr, defStyleRes, attrs, a.data, a.indices);
            a.theme = this;
            a.xml = parser;
            return a;
        }

        public TypedArray resolveAttributes(int[] values, int[] attrs) {
            TypedArray a = TypedArray.obtain(Resources.this, attrs.length);
            assets.resolveAttrs(handle, 0, 0, values, attrs, a.data, a.indices);
            a.theme = this;
            return a;
        }

        public android.graphics.drawable.Drawable getDrawable(int id) throws NotFoundException {
            return Resources.this.getDrawable(id, this);
        }

        public int getChangingConfigurations() {
            return 0;
        }

        public int getExplicitStyle(AttributeSet set) {
            return set != null ? set.getStyleAttribute() : 0;
        }

        public int[] getAttributeResolutionStack(int defStyleAttr, int defStyleRes, int explicitStyleRes) {
            return new int[0];
        }

        public void dump(int priority, String tag, String prefix) {}

        @Override
        public String toString() {
            int[] styles = assets.getThemeStyles(handle);
            StringBuilder b = new StringBuilder("Theme{");
            for (int i = 0; styles != null && i < styles.length; i += 2)
                b.append(i > 0 ? ", " : "").append("0x").append(Integer.toHexString(styles[i]));
            return b.append('}').toString();
        }
    }
}
