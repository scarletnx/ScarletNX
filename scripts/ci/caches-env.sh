#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The compiler and dependency caches' variables, for the rest of the job: build.yml's "Set up the caches"
set -eo pipefail
ws=${GITHUB_WORKSPACE//\\//}
{
  echo "CCACHE_DIR=$ws/.cache/ccache"
  # paths in the keys relative to the checkout, and no build folder in them: build trees share entries
  echo "CCACHE_BASEDIR=$ws"
  echo "CCACHE_NOHASHDIR=true"
  echo "SNX_DEPS_CACHE=$ws/.cache/deps"
  echo "SNX_CCACHE_START=$(date +%s)"
} >> "$GITHUB_ENV"
