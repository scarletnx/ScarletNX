// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.service.autofill;

/** A field's value in a dataset */
public final class Field {
    private Field() {}
    public static final class Builder {
        public Builder() {}
        public Builder setValue(android.view.autofill.AutofillValue value) { return this; }
        public Builder setPresentations(Presentations presentations) { return this; }
        public Field build() { return new Field(); }
    }
}
