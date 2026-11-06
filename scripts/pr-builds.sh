#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Links a pull request's builds from one comment, which .github/workflows/pr-checks.yml rewrites each time a run of
# .github/workflows/build.yml on it ends, so they can be had from the pull request itself. A run of a commit that is no
# longer the branch's head leaves the comment alone.
#
#   scripts/pr-builds.sh REPO PR RUN SHA [--print]
#
# PR may be empty: then it is the open pull request whose head is SHA, and with none there is nothing to link. A job of
# the run that did not succeed gets a line, by its id in build.yml, but edenprobe, which builds nothing: its line is its
# result, the notice its job leaves, linked to the run's summary (docs/EDENPROBE.md). The checks of a package as
# installed and the sanitizers' build make no package, so they get none. NEEDS, when set, stands for the run's jobs, in
# the shape of toJSON(needs). --print writes the comment to stdout and posts nothing. gh needs a token that may read
# the run's artifacts, jobs and checks, and write on pull requests.
set -euo pipefail

if [ $# -lt 4 ] || [ $# -gt 5 ] || { [ $# -eq 5 ] && [ "$5" != --print ]; }; then
    echo "usage: $0 REPO PR RUN SHA [--print]" >&2
    exit 2
fi
repo=$1 pr=$2 run=$3 sha=$4 print=${5:-}
mark='<!-- scarletnx-builds -->'

if [ -z "$pr" ]; then
    pr=$(gh api "repos/$repo/commits/$sha/pulls" |
        jq -r --arg sha "$sha" '[.[] | select(.state == "open" and .head.sha == $sha)][0].number // empty')
    if [ -z "$pr" ]; then
        echo "no open pull request of $repo has ${sha::7} as its head: nothing to link"
        exit 0
    fi
fi
if [ -z "$print" ] && [ "$(gh api "repos/$repo/pulls/$pr" --jq .head.sha)" != "$sha" ]; then
    echo "$sha is no longer the head of #$pr: its comment stays"
    exit 0
fi
# every artifact but the APK's halves (-stage), which only the job that packs the APK takes, and edenprobe's reports,
# which its line below links through the run
artifacts=$(gh api "repos/$repo/actions/runs/$run/artifacts?per_page=100")
links=$(jq -r --arg url "https://github.com/$repo/actions/runs/$run/artifacts" '
    .artifacts | map(select((.expired | not) and (.name | endswith("-stage") or startswith("edenprobe-") | not))) |
    sort_by(.name)[] |
    "- [\(.name)](\($url)/\(.id)), \(.size_in_bytes / 1048576 | floor) MiB"' <<<"$artifacts")
until=$(jq -r '[.artifacts[].expires_at] | min // empty | .[:10]' <<<"$artifacts")

# The run's jobs as toJSON(needs) would have them: a job's legs under its id, with the worst of their conclusions.
# A matrix job skipped whole has one job, under its name before the matrix filled it in. A job this does not know
# keeps its own name, so its failure still shows
if [ -z "${NEEDS:-}" ]; then
    jobs=$(gh api "repos/$repo/actions/runs/$run/jobs?per_page=100")
    needs=$(jq '
        def build_id:
            if . == "macOS and iOS (arm64)" then "apple"
            elif . == "Android APK" then "android"
            elif startswith("Windows (") then "windows"
            elif startswith("edenprobe") then "edenprobe"
            elif endswith(", as installed") then empty
            elif test("^Linux AppImage \\((x86_64|aarch64)\\)$") then "appimage"
            elif startswith("Linux AppImage (") then "appimage-cross"
            elif test("^Linux \\([^,]*\\)$") then "linux"
            elif startswith("Linux (") then empty
            else . end;
        # the order of the needs build.yml gave the comment, then any other
        ["linux", "appimage", "appimage-cross", "windows", "android", "apple", "edenprobe"] as $needs |
        ["failure", "timed_out", "cancelled", "action_required", "skipped"] as $worst |
        [.jobs[] | {key: (.name | build_id), job: .id, conclusion: (.conclusion // .status)}] | group_by(.key) |
        map((map(.conclusion) | unique) as $c | {key: .[0].key, value: {
            job: .[0].job,
            result: (if $c == ["success"] then "success"
                else ([$worst[] | select(. as $o | $c | any(. == $o))] + ($c - ["success"]))[0] end),
            outputs: {}}}) |
        sort_by(.key as $k | $needs | index($k) // length) |
        from_entries' <<<"$jobs")
    probe_job=$(jq -r '.edenprobe | select(.result == "success" or .result == "failure") | .job // empty' <<<"$needs")
    if [ -n "$probe_job" ]; then
        notice=$(gh api "repos/$repo/check-runs/$probe_job/annotations" |
            jq -r '[.[] | select(.title == "edenprobe")][0].message // empty')
        needs=$(jq --arg notice "$notice" 'if $notice != "" then .edenprobe.outputs.result = $notice else . end' <<<"$needs")
    fi
else
    needs=$NEEDS
fi
failed=$(jq -r 'to_entries[] | select(.key != "edenprobe" and .value.result != "success") |
    "\(.key) (\(.value.result))"' <<<"$needs" | paste -sd, - | sed 's/,/, /g')
probe=$(jq -r --arg url "https://github.com/$repo/actions/runs/$run" '.edenprobe // empty |
    "[\(.outputs.result // "edenprobe")](\($url)): " +
    if .result == "success" then "every case of the baseline passes"
    elif .result == "failure" then "failed, see its summary" else .result end' <<<"$needs")

body="$mark
Builds of ${sha::7}${until:+, kept until $until}:
${links:-none}${probe:+

$probe}${failed:+

Not built: $failed}"

if [ -n "$print" ]; then
    printf '%s\n' "$body"
    exit 0
fi
id=$(gh api --paginate "repos/$repo/issues/$pr/comments?per_page=100" --jq \
    ".[] | select(.user.login == \"github-actions[bot]\" and (.body | startswith(\"$mark\"))) | .id" | head -n1)
if [ -n "$id" ]; then
    gh api -X PATCH "repos/$repo/issues/comments/$id" -f body="$body" > /dev/null
else
    gh api "repos/$repo/issues/$pr/comments" -f body="$body" > /dev/null
fi
echo "linked the builds of $sha from #$pr"
