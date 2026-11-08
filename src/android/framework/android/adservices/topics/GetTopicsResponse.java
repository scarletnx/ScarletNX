// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
package android.adservices.topics;

import java.util.List;

/** The topics of the user's interests. */
public final class GetTopicsResponse {
    private final List<Topic> topics;

    private GetTopicsResponse(List<Topic> topics) { this.topics = topics; }
    public List<Topic> getTopics() { return topics; }

    public static final class Builder {
        private final List<Topic> topics;
        public Builder(List<Topic> topics) { this.topics = topics; }
        public GetTopicsResponse build() { return new GetTopicsResponse(List.copyOf(topics)); }
    }
}
