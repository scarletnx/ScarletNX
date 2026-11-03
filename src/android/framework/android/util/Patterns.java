// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.util;

import java.util.regex.Pattern;

/** Patterns of common text: e-mail addresses, IP addresses, host names and web URLs. */
public class Patterns {
    private Patterns() {}

    private static final String OCTET = "(?:25[0-5]|2[0-4][0-9]|1[0-9]{2}|[1-9]?[0-9])";
    private static final String IPV4 = OCTET + "(?:\\." + OCTET + "){3}";
    private static final String LABEL = "[\\p{L}\\p{N}](?:[\\p{L}\\p{N}\\-]{0,61}[\\p{L}\\p{N}])?";
    private static final String HOST = "(?:" + LABEL + "\\.)+[\\p{L}]{2,63}";

    public static final Pattern IP_ADDRESS = Pattern.compile(IPV4);
    public static final Pattern DOMAIN_NAME = Pattern.compile("(?:" + HOST + "|" + IPV4 + ")");
    public static final Pattern EMAIL_ADDRESS = Pattern.compile(
        "[A-Za-z0-9+._%\\-]{1,256}@[A-Za-z0-9][A-Za-z0-9\\-]{0,64}(?:\\.[A-Za-z0-9][A-Za-z0-9\\-]{0,25})+");
    public static final Pattern WEB_URL = Pattern.compile(
        "(?:(?i:https?|rtsp|ftp)://(?:[^\\s@/]{1,64}@)?)?"
        + "(?:" + HOST + "|" + IPV4 + ")"
        + "(?::\\d{1,5})?"
        + "(?:[/?#][^\\s]*)?"
        + "(?=\\b|$|\\s)");
    public static final Pattern PHONE = Pattern.compile("(?:\\+[0-9]+[\\- .]*)?(?:\\([0-9]+\\)[\\- .]*)?[0-9][0-9\\- .]{3,}[0-9]");
}
