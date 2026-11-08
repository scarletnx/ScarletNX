<!--
SPDX-License-Identifier: GPL-3.0-or-later
Copyright 2026 the ScarletNX contributors
The title is `[area] an imperative sentence`, like the commits' (`[gpu] Set the threads making new pipelines`).
While another pull request must merge first, put `[donotmerge needs #N]` in the title and keep the pull request in
draft. The title check fails until the mark is out.
Delete these comments before you open the pull request.
-->
One to three plain sentences: the change and the failure or the gap it closes. Name the game or the system when there
is one. Do not open with a "What", "Why", "Summary" or "Changes" heading. The first sentence is the change itself.

- `[area] commit subject`: why, one bullet for each commit. A pull request of one commit can skip this list.

Testing: what you ran and what it showed. A test that fails without the change is the best proof. For a runtime change, the game, the scene and how many runs.
Not tested: what you did not check. Leave the line out if there is nothing.
Risks: a known regression with its numbers, or `None known`.

Fixes #
<!-- `Part of #n` when this is one step of several. -->
Depends on: #N, in merge order. Leave the line out if there is nothing.

- [ ] No keys, dumps, game files or saves in the commits or the description.
- [ ] A new file starts with `SPDX-License-Identifier: GPL-3.0-or-later` and the project's copyright line.
- [ ] Code ported from another project keeps its copyright lines, names where it came from and is listed in `docs/CREDITS.md`.
