/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_FAULT_H
#define SNX_ANDROID_FAULT_H
/* A host fault (SIGSEGV, SIGBUS, SIGILL, SIGFPE, or a Windows exception) on a thread that runs the guest's generated
   code ends the app: the bridge first says the guest's registers, with the library and offset of each address in
   guest code and a backtrace by the guest's frame records, then gives the fault to the JVM's handler, which writes
   its own report. Every other fault (the JVM's own null checks among them) goes to the JVM's handler alone. Installed
   once, after the JVM installed its handlers: at the first native library's load */
void fault_install(void);
#endif
