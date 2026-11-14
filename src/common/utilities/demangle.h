/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A game's C++ symbol in the form its source wrote it, through the C++ runtime's demangler (demangle.c). A program
   that links it links the C++ runtime (LINKER_LANGUAGE CXX), as every ScarletNX program does for lunasvg */
#ifndef SNX_DEMANGLE_H
#define SNX_DEMANGLE_H
/* name ("_ZN2nn3hid18GetNpadJoyHoldTypeEv") demangled, malloc'd, or NULL for a name that isn't a mangled one */
char* snx_demangle(const char* name);
#endif
