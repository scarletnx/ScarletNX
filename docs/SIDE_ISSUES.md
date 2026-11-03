# Side issue ledger

Every open issue that is not part of the HDR, upscaling, anti aliasing or frame interpolation work goes here. The
work is off the latest `origin/main`, one branch per issue, easiest first. Nothing is pushed. The user or another
agent can take a row and make the pull request from the notes.

## Workflow

1. `git fetch origin main`
2. `git switch -c fix/issue-<number>-<short> origin/main`
3. Fix the smallest complete cause. Follow `CLAUDE.md` and `docs/issues/` practices.
4. Build, run the tests and, where a game or device is needed, run it. Put the commands and the result in the row.
5. Commit signed. Do not push.
6. Fill the row. The suggested PR title starts with the area, as the repository's commits do.

## Ledger

| Issue | Title | Rank | Branch | Base | Status | Notes / mechanism | Tests / run | Suggested PR |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
|  |  |  |  |  |  |  |  |  |

## Blocked or not for this device

| Issue | Why | Next step |
| --- | --- | --- |
|  |  |  |

## Main feature branch

The HDR, upscaling, anti aliasing, frame interpolation and `docked_key` work is on
`greatgreenstar/feat/hdr-780`, based on PR #1104. Its checklist is `docs/REMAINING.md`, its defects are in
`docs/issues/`, and its runnable build is in
`/home/greenstar/Downloads/compiled_scarlet/Hotkey_stuff_forALL/`. The four local commits are also in
`patches/` there. Nothing was pushed.
