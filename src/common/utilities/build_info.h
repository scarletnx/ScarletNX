/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_BUILD_INFO_H
#define SNX_BUILD_INFO_H
/* "ScarletNX <branch> <commit> (<compiler>, <OS> <arch>)", for the window titles */
const char* snx_build_info(void);
/* the logs' line, snx_build_info's with the day its commit was made (UTC): "ScarletNX <branch> <commit>, committed
   <YYYY-MM-DD> (<compiler>, <OS> <arch>)", "committed unknown" outside a git checkout */
const char* snx_build_log_line(void);
/* the logs' line of when a run began, now in seconds since 1970: "run: started <YYYY-MM-DD> <HH:MM:SS> UTC". The log's
   own times count from the run's start and its file's date is lost when it is shared, so this line is what says
   how old the build was when it ran (common/utilities/logscan.c) */
#define SNX_RUN_LOG_LINE 40
void snx_run_log_line(char out[SNX_RUN_LOG_LINE], long long now);
/* the commit in it: a short hash, "-dirty" after it when the tree had changes, "unknown" outside a git checkout */
const char* snx_build_commit(void);
#endif
