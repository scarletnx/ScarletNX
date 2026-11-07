// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.view.autofill;

/** A view's autofill identity. */
public final class AutofillId implements android.os.Parcelable {
    private final int viewId, virtualId;
    public AutofillId(int id) { this(id, -1); }
    public AutofillId(AutofillId hostId, int virtualChildId) { this(hostId.viewId, virtualChildId); }
    private AutofillId(int viewId, int virtualId) { this.viewId = viewId; this.virtualId = virtualId; }
    public static AutofillId create(android.view.View host, int virtualId) { return new AutofillId(host.getAutofillId(), virtualId); }
    @Override public boolean equals(Object o) { return o instanceof AutofillId a && a.viewId == viewId && a.virtualId == virtualId; }
    @Override public int hashCode() { return viewId * 31 + virtualId; }
    @Override public String toString() { return viewId + (virtualId >= 0 ? ":" + virtualId : ""); }
    @Override public int describeContents() { return 0; }
    @Override public void writeToParcel(android.os.Parcel out, int flags) { out.writeInt(viewId); out.writeInt(virtualId); }
}
