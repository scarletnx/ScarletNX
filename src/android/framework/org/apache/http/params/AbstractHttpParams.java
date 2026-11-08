// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package org.apache.http.params;

public abstract class AbstractHttpParams implements HttpParams {
    protected AbstractHttpParams() {}
    @Override public long getLongParameter(String name, long d) { return getParameter(name) instanceof Number n ? n.longValue() : d; }
    @Override public HttpParams setLongParameter(String name, long value) { return setParameter(name, value); }
    @Override public int getIntParameter(String name, int d) { return getParameter(name) instanceof Number n ? n.intValue() : d; }
    @Override public HttpParams setIntParameter(String name, int value) { return setParameter(name, value); }
    @Override public double getDoubleParameter(String name, double d) { return getParameter(name) instanceof Number n ? n.doubleValue() : d; }
    @Override public HttpParams setDoubleParameter(String name, double value) { return setParameter(name, value); }
    @Override public boolean getBooleanParameter(String name, boolean d) { return getParameter(name) instanceof Boolean b ? b : d; }
    @Override public HttpParams setBooleanParameter(String name, boolean value) { return setParameter(name, value); }
    @Override public boolean isParameterTrue(String name) { return getBooleanParameter(name, false); }
    @Override public boolean isParameterFalse(String name) { return !getBooleanParameter(name, false); }
}
