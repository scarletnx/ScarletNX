---
name: snx-work-mark
description: "Use it when an agent opens a pull request, answers an issue or closes one. It gives the title mark, the draft rule, the body, the evidence and the reply."
---

# Work mark

An agent leaves a mark on each piece of work. A person must judge the work from the mark alone: what changed, what
proved it, what remains, and what must merge first. This skill gives the mark for a pull request, an issue reply and a
close. It follows the conventions of the ScarletNX contributors, with one addition: an agent names itself.

Read `CLAUDE.md`, `docs/TESTING.md` and `.github/PULL_REQUEST_TEMPLATE.md` first.

## The title

The title is `[area] an imperative sentence`. The areas are in `scripts/area-labels.py`. The workflow adds the labels
from the title, thus a clear area gives the right labels.

A pull request that needs another one first holds `[donotmerge needs #N]` in the title. The check
`scripts/pr-title.py` fails until the mark is out. Keep the pull request in draft until the piece that it needs
merges. Then rebase, take the mark out, and ask for the review again.

The body names the whole chain, in merge order:

```
Depends on: #1261, then #1263, then #1264. Merge in this order.
```

A later pull request in the chain names the one before it, not only the first.

## The draft

A draft tells the reader that the work is not ready. Keep the pull request in draft while a known failure remains.
Write that failure in the body with its size: the case, the numbers and the plan. Remove the draft flag when the
failure stops. Also remove it when the body says why the work can merge with the failure.

## The body

Use this order. Leave out a line only where the template says so.

1. The symptom and the change, in one to three sentences. Name the game or the system.
2. One bullet for each commit: `[area] subject`, then what it does and why.
3. `Depends on:` and `Conflicts:` with the pull requests and files. Say how to resolve a conflict.
4. `Testing:` the exact command, the test names, the result, and the failure on the base commit.
5. `Not tested:` what the run did not reach.
6. `Risks:` a known regression with numbers, or `None known`.

The first paragraph answers "what was wrong" before "what you changed". A reader who stops there must still know the
value of the work.

## The evidence

A mark that a person cannot replay is a claim, not evidence.

- Give the base commit and the head commit.
- Give the command that you ran and the decisive line of its output.
- A green build is not a test. Name the test, the scene or the game run.
- One good run does not prove the fix of an intermittent crash. Give the number of runs.
- A benchmark names the processor, the graphics card, the driver and the build.
- A number from another machine is not a result on this machine.
- Write `not run` where you did not run a check. Never write a result that you did not see.

## The reply

An agent writes as an agent. A contributor's agent writes `From <its name>:`. This skill adds `(an agent)`, so no
reader mistakes the writer for a person. Start a reply on a thread that another person owns with `From <agent
name> (an agent):`.

Answer the exact question first. Then give the evidence. Then say what remains. A reply has this shape:

```
From <agent name> (an agent): <the answer in one sentence>.

Evidence: <the commit, the issue, the log line or the test>.
Not checked: <what this reply does not cover>.
Next: <the action, and who does it>.
```

- Cite the issue, the pull request and the commit hash. A merged pull request gets its number and its commit.
- Ask a person for the one fact that only they have. Do not ask for permission to do the obvious next step.
- Thank the reporter for the fact that helped, not for the report in general.
- Do not write "fixed" before the fix merges. Write "the fix is in #N", or "fixed by #N" after the merge.
- Do not close an issue on a guess. Close it on a confirmation, a merged fix, or a duplicate with its number.

## The close

A close says what closed it. Name the pull request and the commit, or the confirmation and its date. If part of the
report remains, open an issue for that part and link it. Then close.

## The handoff

An agent that gives work to another agent uses the shape of the reply: result, evidence, not checked, next step. The
agent that gets the work must not derive again a fact that the first agent verified. It must not trust a fact that the
first agent only inferred. The label `HYPOTHESIS` marks an inference.

## The checks

Run these before the review:

```sh
python3 scripts/pr-title.py "$TITLE"
python3 scripts/area-labels.py "$TITLE"
python3 .claude/skills/ste100/check.py --text < report.md
```

Assign the owner of the unmerged work that the change touches. The check `scripts/pr-assignees.py` holds the merge
until that person approves. Do not approve your own pull request. Do not merge it.
