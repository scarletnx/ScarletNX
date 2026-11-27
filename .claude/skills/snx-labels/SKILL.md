---
name: snx-labels
description: "Put one kind, one priority and one preview effort on an issue or a pull request of ScarletNX. Choose them from what the change does and what it touches. The skill also tells which labels the workflows add from the title, and which labels only a maintainer adds. Use it when you open an issue or a pull request, or when the user asks for labels."
---

# Labels

Each open issue and pull request has three labels that you choose: one `kind:`, one `priority:` and one
`preview-effort:`. The workflows add the `area:` and `platform:` labels from the title.

Add labels only. Do not remove or replace a label that a person or a workflow added. Remove a label only when the
user tells you to.

## 1. The labels of the repository

```sh
gh label list --repo scarletnx/ScarletNX --limit 200 --json name,description \
    --jq '.[] | "\(.name)\t\(.description)"'
```

Use only a label that this list shows. Do not make a new label. The description of a label is its definition. When a
description and this skill do not agree, use the description.

## 2. The labels that the workflows add

The title starts with an area prefix in brackets, then an imperative sentence:
`[gpu] Keep the scratch buffer when the heap is full`. A prefix can name more than one area, as in `[macos, linux]`.

When an issue or a pull request opens, and when its title changes, `.github/workflows/pr-checks.yml` runs
`scripts/area-labels.py` on the title. The workflow adds each label that the script prints:

- The `area:` or `platform:` label of each area in the prefix. An area with no label, as in `[x64]`, gives no label.
- `kind: documentation` for the prefix `[docs]`.
- One kind from the words after the prefix. A title that starts with "Fix", or that names a crash or a hang, gives
  `kind: bug`. A title that starts with a verb such as "Add" or "Support" gives `kind: enhancement`. This kind is a guess.

Do not add an `area:` or a `platform:` label by hand. For a different area label, change the prefix of the title. The
workflow then adds the labels of the new prefix, but it does not remove the old labels. Tell the user about an old
label that is wrong.

Before you choose a kind, run the script on the title. It prints the labels that the workflow will add:

```sh
python3 scripts/area-labels.py "[gpu] Fix the depth bias of a split draw"
```

Run it again on each new title.

Other workflows and the maintainers add these labels. Do not add them by hand:

- `game report` and one `log:` label: the game report template and `.github/workflows/game-reports.yml` put them on
  an issue with a ScarletNX log.
- `publish builds`: a member of the team puts it on a pull request from a fork, to publish its builds.
- `status: confirmed`: a maintainer puts it on a game report when the maintainer understands the failure.

## 3. Kind

Give one kind. Choose it from the main purpose of the change, or from the problem that the issue tells.

| Label | Description | Use it for |
| --- | --- | --- |
| `kind: bug` | Something does not work | A crash, a hang, a wrong result, or a build or a test that fails. The fix of one of these. |
| `kind: enhancement` | New feature or improvement | New support, a new option, more speed, less memory, a new test or a new tool. |
| `kind: documentation` | Docs only | Docs, comments or the Claude files, and no change to the code. |
| `kind: accessibility` | A barrier for people with disabilities | A barrier of this type, or its removal. |

Add a kind only when the script prints no kind and the issue or the pull request has no kind. When the kind of the
script is wrong, or when the script prints two kinds, tell the user. Do not add a second kind.

## 4. Priority

Give one priority. The priority of an issue follows its effect on the users. A pull request takes the priority of
the issue that it fixes. When it fixes more than one issue, it takes the highest priority of the issues.

| Label | Description | Use it for |
| --- | --- | --- |
| `priority: urgent` | Fix before anything else | `main` does not build, or a check fails on `main`. A change on `main` stops all games, or loses the data of a user. |
| `priority: high` | Next in line | A crash, a hang, or a game that does not start. A regression. A change that other work waits for. |
| `priority: medium` | Planned | Most fixes and features: a wrong result in one game or one scene, a feature that an issue asks for. |
| `priority: low` | When there is time | A message, a label or a small fault on the screen. A cleanup, a test that does not fail now, a small gain. |

## 5. Preview effort

For a pull request, the preview effort is the size of the review. For an issue, it is the size of the work to fix it.

| Label | Description | Use it for |
| --- | --- | --- |
| `preview-effort: small` | A contained change | One function or one file, or a few lines in a few files for one purpose. |
| `preview-effort: medium` | A focused change across a few files | A few files in one subsystem, with the tests of the change. |
| `preview-effort: large` | Days of work or a redesign | Many files or subsystems, a new design, or a change to a file format or an ABI. |

Count the files and the subsystems that a reviewer must read, not only the lines. `git diff --stat origin/main...HEAD`
shows them. Generated files and test data add little to the review.

## 6. Other labels

- `good first issue` and `help wanted`: put them on an issue when their description is true for it.
- `status: needs info`, `status: duplicate`, `status: invalid` and `status: wontfix`: each is the decision of a
  maintainer. Add one only when the user tells you to.

## 7. Issue types

An issue also has a type: `Bug`, `Feature` or `Task`. A pull request has no type. Give a type to an issue that has
none: `Bug` with `kind: bug`, `Feature` with `kind: enhancement`, and `Task` for other work. Do not change a type that
a person chose.

## 8. Commands

```sh
# a new pull request: --label "kind: ..." only when area-labels.py prints no kind for the title
gh pr create --repo scarletnx/ScarletNX --title "[gpu] ..." --body-file BODY.md \
    --label "kind: bug" --label "priority: medium" --label "preview-effort: small"

# a new issue
gh issue create --repo scarletnx/ScarletNX --title "[gpu] ..." --body-file BODY.md --type Bug \
    --label "kind: bug" --label "priority: high" --label "preview-effort: medium"

# an open pull request or issue: read its labels (and the type of an issue) first, then add what it does not have
gh pr view N --repo scarletnx/ScarletNX --json labels --jq '.labels[].name'
gh pr edit N --repo scarletnx/ScarletNX --add-label "priority: medium,preview-effort: small"
gh issue view N --repo scarletnx/ScarletNX --json labels,issueType
gh issue edit N --repo scarletnx/ScarletNX --add-label "kind: bug,priority: high,preview-effort: medium"
gh issue edit N --repo scarletnx/ScarletNX --type Bug
```

A label needs the triage role on the repository. Without it, GitHub does not add the label, or `gh` gives an error.
Read the labels after each command. When a label is not there, give the user the labels that you chose, with the
error, so that a maintainer can add them.
