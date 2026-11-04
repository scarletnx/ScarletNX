# `TriggerCmdReqQueue` answered unsupported when the right and the relay came from two sessions

## What was seen

A title acquired the GPU right on one `gsp::Gpu` session and registered its interrupt relay on another. The
process had the right and a relay queue, but `TriggerCmdReqQueue` looked up the relay thread on the session that
held the right, which had none, and set the request unsupported.

## Fix

`SnxCtrGsp` keeps `relay_thread`, the registered relay queue of the process, apart from `rights_thread`, the session
that holds the GPU right. `RegisterInterruptRelayQueue` sets it, `UnregisterInterruptRelayQueue` and the session close
clear it, and `TriggerCmdReqQueue` and the VRAM save/restore commands use it. With no relay yet, the command answers
success as hardware does, because there is no queue to run.

## Where

`src/3ds/runtime/internal.h`, `src/3ds/runtime/process.c`, `src/3ds/services/gsp.c`.
