/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_EXTRACT_H
#define SNX_EXTRACT_H

#include "common/utilities/common.h"

/* argv contains extraction arguments only, without a program name or --extract. */
int snx_extract(int argc, char** argv, const SnxPaths* paths);
/* The title ID of dump's game, as -x would extract it (the keys where -x finds them), read from its content meta and
   NCA headers alone: what a dump's game folder is named by before it is extracted into it, in title and game both. A
   dump of an update or a DLC alone gives the add-on's own in title, and in game the game its content meta names it
   of (snx_addon_kind(*game, *title) tells which it is), with its version in *version when version is not NULL
   (SNX_NO_VERSION when unknown), and how many programs its application (or the update's patch) has in *programs when
   programs is not NULL (container.h's SnxDump.programs). 0, else nonzero, said why */
int snx_extract_title(const char* dump, const SnxPaths* paths, SnxU64* title, SnxU64* game, SnxU64* version,
                      int* programs);
/* The icon of dump's game alone, as -x keeps it, into out/icon.jpg: for a game made before its extraction kept one.
   0, else nonzero, said why (no Control NCA, no icon in it, a file that cannot be written) */
int snx_extract_icon(const char* dump, const SnxPaths* paths, const char* out);

#endif
