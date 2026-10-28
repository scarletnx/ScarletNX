/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Every struct g_hle holds, for what walks it with Clang's __builtin_dump_struct: tests/hle_flat.c, which keeps it
   flat, and state.c's schema hash, which changes with any of its fields. A pool's slots and an array's elements are
   walked by their type (the builtin doesn't go into arrays), each member of g_hle last. HLE_WALKS(X, h) calls
   X(name, lvalue) for each, h being an Hle. A struct g_hle gets an array of must be added here: hle_flat fails on an
   array of a struct no walk covers */
#ifndef SNX_HLE_WALK_H
#define SNX_HLE_WALK_H
#define HLE_WALKS(X, h)                                                                                                \
    X(KThread, (h).thread.threads[0])                                                                                  \
    X(KEvent, (h).kernel.events[0])                                                                                    \
    X(HEntry, (h).kernel.htab[0])                                                                                      \
    X(SvcRec, (h).kernel.svc_ring[0])                                                                                  \
    X(KWEvent, (h).svc.wevents[0])                                                                                     \
    X(KShmem, (h).svcmem.shmems[0])                                                                                    \
    X(KTmem, (h).svcmem.tmems[0])                                                                                      \
    X(KCodeMem, (h).svcmem.codemems[0])                                                                                \
    X(KCodeView, (h).svcmem.views[0])                                                                                  \
    X(KMapView, (h).svcmem.maps[0])                                                                                    \
    X(debug_sleeps_slot, (h).debug.sleeps[0])                                                                          \
    X(SvcObj, (h).ipc.svcobjs[0])                                                                                      \
    X(KSession, (h).ipc.sessions[0])                                                                                   \
    X(Domain, (h).ipc.domains[0])                                                                                      \
    X(ipc_seen_slot, (h).ipc.seen[0])                                                                                  \
    X(SvcReg, (h).ipc.reg[0])                                                                                          \
    X(IpcRec, (h).ipc.ring[0])                                                                                         \
    X(AccUser, (h).acc.users[0])                                                                                       \
    X(StorageData, (h).am.storages[0])                                                                                 \
    X(AmAccessor, (h).am.accessors[0])                                                                                 \
    X(AmVolumes, (h).am.volumes[0])                                                                                    \
    X(AmLock, (h).am.lock_objs[0])                                                                                     \
    X(LibApplet, (h).am.applets[0])                                                                                    \
    X(AudOut, (h).audio.outs_pool[0])                                                                                  \
    X(AudRen, (h).audio.rens[0])                                                                                       \
    X(AudDevice, (h).audio.devices[0])                                                                                 \
    X(HwOpus, (h).audio.opus[0])                                                                                       \
    X(ARRen, (h).audren.rens[0])                                                                                       \
    X(ARCompressor, (h).audren_dsp.compressors[0])                                                                     \
    X(PadMotion, (h).hid.pad1.motion[0])                                                                               \
    X(PadState, (h).hid.pad[0])                                                                                        \
    X(HidMotionState, (h).hid.motion[0][0])                                                                            \
    X(HidVibration, (h).hid.vibration[0][0])                                                                           \
    X(RoState, (h).ldr.states[0])                                                                                      \
    X(RoNro, (h).ldr.states[0].nro[0])                                                                                 \
    X(RoNrr, (h).ldr.states[0].nrr[0])                                                                                 \
    X(MmRequest, (h).misc.mms[0].req[0])                                                                               \
    X(NvEvent, (h).nvdrv.ctrl_evt[0].slot[0])                                                                          \
    X(ViSlot, (h).vi.layer[0].slot[0])                                                                                 \
    X(BsdFd, (h).bsd.bsds[0].fd[0])                                                                                    \
    X(Mm, (h).misc.mms[0])                                                                                             \
    X(NfcUser, (h).misc.nfc_users[0])                                                                                  \
    X(NvMapH, (h).nvdrv.nvh[0])                                                                                        \
    X(NvEventTable, (h).nvdrv.ctrl_evt[0])                                                                             \
    X(NvFile, (h).nvdrv.nvf[0])                                                                                        \
    X(NvSeen, (h).nvdrv.seen[0])                                                                                       \
    X(TimeObj, (h).time.objs[0])                                                                                       \
    X(ViLayer, (h).vi.layer[0])                                                                                        \
    X(Bsd, (h).bsd.bsds[0])                                                                                            \
    X(Ldn, (h).ldn.ldn[0])                                                                                             \
    X(NifmRequest, (h).nifm.reqs[0])                                                                                   \
    X(SslContext, (h).ssl.ctxs[0])                                                                                     \
    X(SslConn, (h).ssl.conns[0])                                                                                       \
    X(ROpen, (h).fs.openf[0])                                                                                          \
    X(FsData, (h).fs.fss[0])                                                                                           \
    X(FileData, (h).fs.files[0])                                                                                       \
    X(DirData, (h).fs.dirs[0])                                                                                         \
    X(kernel, (h).kernel)                                                                                              \
    X(thread, (h).thread)                                                                                              \
    X(sync, (h).sync)                                                                                                  \
    X(debug, (h).debug)                                                                                                \
    X(svc, (h).svc)                                                                                                    \
    X(svcmem, (h).svcmem)                                                                                              \
    X(ipc, (h).ipc)                                                                                                    \
    X(acc, (h).acc)                                                                                                    \
    X(am, (h).am)                                                                                                      \
    X(audio, (h).audio)                                                                                                \
    X(audren, (h).audren)                                                                                              \
    X(audren_dsp, (h).audren_dsp)                                                                                      \
    X(hid, (h).hid)                                                                                                    \
    X(ldr, (h).ldr)                                                                                                    \
    X(misc, (h).misc)                                                                                                  \
    X(nvdrv, (h).nvdrv)                                                                                                \
    X(pl, (h).pl)                                                                                                      \
    X(set, (h).set)                                                                                                    \
    X(sm, (h).sm)                                                                                                      \
    X(time, (h).time)                                                                                                  \
    X(vi, (h).vi)                                                                                                      \
    X(bsd, (h).bsd)                                                                                                    \
    X(ldn, (h).ldn)                                                                                                    \
    X(nifm, (h).nifm)                                                                                                  \
    X(ssl, (h).ssl)                                                                                                    \
    X(fs, (h).fs)

#endif
