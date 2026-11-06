# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The branch and commit of SRC's tree, for the window titles: the branch checked out (CI checks a pull request out
# detached, so GITHUB_HEAD_REF, then GITHUB_REF_NAME; else "detached"), and the short hash with "-dirty" when the work
# tree differs; both "unknown" outside a git checkout. And the day that commit was made, for the logs: its committer
# date in UTC, YYYY-MM-DD, "unknown" outside a git checkout too. Writes OUT only when that changed, so a commit relinks
# the two programs that show it and nothing else. With CHECK set, compares OUT with git instead, as the
# build_info_commit test
execute_process(COMMAND git -C "${SRC}" describe --always --dirty --abbrev=7 --match=snx-no-tag-matches
    OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE failed)
if(failed OR NOT commit)
    set(commit "unknown")
endif()
execute_process(COMMAND git -C "${SRC}" branch --show-current
    OUTPUT_VARIABLE branch OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE branch_failed)
if(commit STREQUAL "unknown" OR branch_failed)
    set(branch "unknown")
elseif(NOT branch AND DEFINED ENV{GITHUB_HEAD_REF} AND NOT "$ENV{GITHUB_HEAD_REF}" STREQUAL "")
    set(branch "$ENV{GITHUB_HEAD_REF}")
elseif(NOT branch AND DEFINED ENV{GITHUB_REF_NAME} AND NOT "$ENV{GITHUB_REF_NAME}" STREQUAL "")
    set(branch "$ENV{GITHUB_REF_NAME}")
elseif(NOT branch)
    set(branch "detached")
endif()
# a C string: a branch name may hold a quote, never a backslash (git refuses those)
string(REPLACE "\"" "\\\"" branch "${branch}")
execute_process(COMMAND git -C "${SRC}" log -1 --format=%ct HEAD
    OUTPUT_VARIABLE seconds OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE date_failed)
if(commit STREQUAL "unknown" OR date_failed OR NOT seconds MATCHES "^[0-9]+$")
    set(date "unknown")
else()
    # string(TIMESTAMP) tells the time SOURCE_DATE_EPOCH names, in place of now
    set(ENV{SOURCE_DATE_EPOCH} "${seconds}")
    string(TIMESTAMP date "%Y-%m-%d" UTC)
endif()
if(CHECK)
    file(READ "${OUT}" header)
    execute_process(COMMAND git -C "${SRC}" rev-parse --short=7 HEAD OUTPUT_VARIABLE head
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE failed)
    if(failed OR NOT head)
        set(head "unknown")
    endif()
    if(NOT header MATCHES "#define SNX_COMMIT \"${head}(-dirty)?\"")
        message(FATAL_ERROR "${OUT} says ${header}, the tree is at ${head}")
    endif()
    if(NOT header MATCHES "#define SNX_BRANCH \"[^\n]+\"\n")
        message(FATAL_ERROR "${OUT} names no branch: ${header}")
    endif()
    if(NOT header MATCHES "#define SNX_COMMIT_DATE \"${date}\"\n")
        message(FATAL_ERROR "${OUT} says ${header}, the tree's commit was made on ${date}")
    endif()
    return()
endif()
file(CONFIGURE OUTPUT "${OUT}"
    CONTENT "#define SNX_BRANCH \"@branch@\"\n#define SNX_COMMIT \"@commit@\"\n#define SNX_COMMIT_DATE \"@date@\"\n"
    @ONLY)
