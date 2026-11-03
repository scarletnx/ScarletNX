// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.adservices.measurement;

import android.net.Uri;
import java.time.Instant;
import java.util.List;

/** A request to delete measurement registrations. */
public class DeletionRequest {
    public static final int DELETION_MODE_ALL = 0, DELETION_MODE_EXCLUDE_INTERNAL_DATA = 1;
    public static final int MATCH_BEHAVIOR_DELETE = 0, MATCH_BEHAVIOR_PRESERVE = 1;
    private final int deletionMode, matchBehavior;
    private final Instant start, end;
    private final List<Uri> originUris, domainUris;

    private DeletionRequest(Builder b) {
        deletionMode = b.deletionMode; matchBehavior = b.matchBehavior; start = b.start; end = b.end;
        originUris = List.copyOf(b.originUris); domainUris = List.copyOf(b.domainUris);
    }
    public int getDeletionMode() { return deletionMode; }
    public int getMatchBehavior() { return matchBehavior; }
    public Instant getStart() { return start; }
    public Instant getEnd() { return end; }
    public List<Uri> getOriginUris() { return originUris; }
    public List<Uri> getDomainUris() { return domainUris; }

    public static final class Builder {
        private int deletionMode = DELETION_MODE_ALL, matchBehavior = MATCH_BEHAVIOR_DELETE;
        private Instant start = Instant.MIN, end = Instant.MAX;
        private List<Uri> originUris = List.of(), domainUris = List.of();

        public Builder() {}
        public Builder setDeletionMode(int deletionMode) { this.deletionMode = deletionMode; return this; }
        public Builder setMatchBehavior(int matchBehavior) { this.matchBehavior = matchBehavior; return this; }
        public Builder setStart(Instant start) { this.start = start; return this; }
        public Builder setEnd(Instant end) { this.end = end; return this; }
        public Builder setOriginUris(List<Uri> originUris) { this.originUris = originUris != null ? originUris : List.of(); return this; }
        public Builder setDomainUris(List<Uri> domainUris) { this.domainUris = domainUris != null ? domainUris : List.of(); return this; }
        public DeletionRequest build() { return new DeletionRequest(this); }
    }
}
