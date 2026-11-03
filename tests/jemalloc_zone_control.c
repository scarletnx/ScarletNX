/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A program without jemalloc for the jemalloc test on macOS (tests/jemalloc.cmake): inserted into it, the zone library
   must name the system's zone for a new allocation, or its jemalloc_zone for scarletnx's would prove nothing */
int main(void) { return 0; }
