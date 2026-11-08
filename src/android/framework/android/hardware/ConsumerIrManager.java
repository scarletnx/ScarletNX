// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.hardware;

/** The infrared transmitter of a device that has none. */
public final class ConsumerIrManager {
    public ConsumerIrManager() {}

    public boolean hasIrEmitter() { return false; }
    public void transmit(int carrierFrequency, int[] pattern) {}
    public CarrierFrequencyRange[] getCarrierFrequencies() { return null; }

    public final class CarrierFrequencyRange {
        private final int min, max;
        public CarrierFrequencyRange(int min, int max) { this.min = min; this.max = max; }
        public int getMinFrequency() { return min; }
        public int getMaxFrequency() { return max; }
    }
}
