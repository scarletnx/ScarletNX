// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.adservices.topics;

/** A topic of the user's interests, in a taxonomy. */
public final class Topic {
    private final long taxonomyVersion, modelVersion;
    private final int topicId;

    public Topic(long taxonomyVersion, long modelVersion, int topicId) { this.taxonomyVersion = taxonomyVersion; this.modelVersion = modelVersion; this.topicId = topicId; }
    public long getTaxonomyVersion() { return taxonomyVersion; }
    public long getModelVersion() { return modelVersion; }
    public int getTopicId() { return topicId; }
    @Override public boolean equals(Object o) { return o instanceof Topic t && t.taxonomyVersion == taxonomyVersion && t.modelVersion == modelVersion && t.topicId == topicId; }
    @Override public int hashCode() { return java.util.Objects.hash(taxonomyVersion, modelVersion, topicId); }
}
