/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SceCommonDialog: the system's dialogs, drawn over the game. One is open at a time: its Init opens it (BUSY while
   another is open), its GetStatus says RUNNING until the player is done with it, then FINISHED, its GetResult gives
   what came of it, and its Term closes it. A game waits for FINISHED, so a dialog the runtime doesn't model would
   stall it: the ones modeled here finish as they open, with what the console's would end with when nothing needs
   the player. The trophy setup dialog (the trophies' files made ready) succeeds. The network check dialog finds no
   network (the runtime gives a game none: vita/hle/net.c), and the player backs out of its message. A finished
   dialog keeps the others out until its Term: a game that opens the network check as it boots and closes it from
   its menus waits that long for its trophy setup, retrying its Init while it answers BUSY */
#include "vita/hle/hle.h"
#include "vita/hle/hle.hook.h"
#include <string.h>

#define SCE_COMMON_DIALOG_ERROR_BUSY         0x80020401u
#define SCE_COMMON_DIALOG_ERROR_NULL         0x80020402u
#define SCE_COMMON_DIALOG_ERROR_NOT_RUNNING  0x80020404u
#define SCE_COMMON_DIALOG_ERROR_NOT_FINISHED 0x80020410u
#define SCE_COMMON_DIALOG_ERROR_NOT_IN_USE   0x80020411u

enum { STATUS_NONE, STATUS_RUNNING, STATUS_FINISHED };
enum { RESULT_OK, RESULT_USER_CANCELED, RESULT_ABORTED };
enum { DIALOG_NONE, DIALOG_TROPHY_SETUP, DIALOG_NET_CHECK, DIALOG_SAVE_DATA };
/* each dialog's result struct: its SceCommonDialogResult first, the rest zero (SceNpTrophySetupDialogResult,
   SceNetCheckDialogResult with psnModeSucceeded false) */
#define RESULT_SIZE 0x84u

static uint32_t init(VitaProcess* p, uint32_t param, uint32_t kind, uint32_t result) {
    if (!param) return SCE_COMMON_DIALOG_ERROR_NULL;
    if (p->dialog.kind) return SCE_COMMON_DIALOG_ERROR_BUSY;
    p->dialog.kind = kind, p->dialog.status = STATUS_FINISHED, p->dialog.result = result;
    return 0;
}
static uint32_t status(VitaProcess* p, uint32_t kind) {
    return p->dialog.kind == kind ? p->dialog.status : STATUS_NONE;
}
static uint32_t result(VitaProcess* p, uint32_t out, uint32_t kind) {
    if (p->dialog.kind != kind) return SCE_COMMON_DIALOG_ERROR_NOT_IN_USE;
    if (p->dialog.status != STATUS_FINISHED) return SCE_COMMON_DIALOG_ERROR_NOT_FINISHED;
    if (!out || !vita_mapped(p, out, RESULT_SIZE)) return SCE_COMMON_DIALOG_ERROR_NULL;
    memset(vita_ptr(p, out), 0, RESULT_SIZE);
    vita_write32(p, out, p->dialog.result);
    return 0;
}
static uint32_t abort_dialog(VitaProcess* p, uint32_t kind) {
    if (p->dialog.kind != kind) return SCE_COMMON_DIALOG_ERROR_NOT_IN_USE;
    if (p->dialog.status != STATUS_RUNNING) return SCE_COMMON_DIALOG_ERROR_NOT_RUNNING;
    p->dialog.status = STATUS_FINISHED, p->dialog.result = RESULT_ABORTED;
    return 0;
}
static uint32_t term(VitaProcess* p, uint32_t kind) {
    if (p->dialog.kind != kind) return SCE_COMMON_DIALOG_ERROR_NOT_IN_USE;
    if (p->dialog.status != STATUS_FINISHED) return SCE_COMMON_DIALOG_ERROR_NOT_FINISHED;
    p->dialog.kind = DIALOG_NONE, p->dialog.status = STATUS_NONE;
    return 0;
}

uint32_t vita_dialog_trophy_setup_init(VitaProcess* p, VitaThread* t, uint32_t param) {
    (void)t;
    return init(p, param, DIALOG_TROPHY_SETUP, RESULT_OK);
}
uint32_t vita_dialog_trophy_setup_status(VitaProcess* p, VitaThread* t) {
    (void)t;
    return status(p, DIALOG_TROPHY_SETUP);
}
uint32_t vita_dialog_trophy_setup_result(VitaProcess* p, VitaThread* t, uint32_t out) {
    (void)t;
    return result(p, out, DIALOG_TROPHY_SETUP);
}
uint32_t vita_dialog_trophy_setup_abort(VitaProcess* p, VitaThread* t) {
    (void)t;
    return abort_dialog(p, DIALOG_TROPHY_SETUP);
}
uint32_t vita_dialog_trophy_setup_term(VitaProcess* p, VitaThread* t) {
    (void)t;
    return term(p, DIALOG_TROPHY_SETUP);
}
uint32_t vita_dialog_net_check_init(VitaProcess* p, VitaThread* t, uint32_t param) {
    (void)t;
    return init(p, param, DIALOG_NET_CHECK, RESULT_USER_CANCELED);
}
uint32_t vita_dialog_net_check_status(VitaProcess* p, VitaThread* t) {
    (void)t;
    return status(p, DIALOG_NET_CHECK);
}
uint32_t vita_dialog_net_check_result(VitaProcess* p, VitaThread* t, uint32_t out) {
    (void)t;
    return result(p, out, DIALOG_NET_CHECK);
}
uint32_t vita_dialog_net_check_abort(VitaProcess* p, VitaThread* t) {
    (void)t;
    return abort_dialog(p, DIALOG_NET_CHECK);
}
uint32_t vita_dialog_net_check_term(VitaProcess* p, VitaThread* t) {
    (void)t;
    return term(p, DIALOG_NET_CHECK);
}

/* ---- sceSaveDataDialog: the console's save and load screens. Nothing here needs the player, so each phase ends as it
   starts, with the answer the player would give (YES, OK). The game drives it: Init opens it (status RUNNING), it polls
   GetSubStatus for FINISHED, reads GetResult, and either Continues to the next screen (a message, a progress bar) or
   Finishes, then polls GetStatus for FINISHED and Terms. SceSaveDataDialogParam: mode at 0x50, the fixed parameter's
   pointer at 0x58 (its first word the slot), the game's userdata at 0x78. SceSaveDataDialogResult: mode, result,
   buttonId, slotId, a SceSaveDataDialogSlotInfo pointer (isExist, slot parameters), userdata */
#define SAVE_BUTTON_YES   1u
#define SAVE_PARAM_MODE   0x50u
#define SAVE_PARAM_FIXED  0x58u
#define SAVE_PARAM_USER   0x78u
#define SAVE_PARAM_SIZE   0x98u
#define SAVE_MODE_FIXED   1u
#define SAVE_MODE_LIST    2u
#define SAVE_ERROR_PARAM  0x80100b01u
static uint32_t save_slot_of(VitaProcess* p, uint32_t param) {
    uint32_t fixed = vita_mapped(p, param, SAVE_PARAM_SIZE) ? vita_read32(p, param + SAVE_PARAM_FIXED) : 0;
    return fixed && vita_mapped(p, fixed, 4) ? vita_read32(p, fixed) : 0;
}
uint32_t vita_dialog_save_data_init(VitaProcess* p, VitaThread* t, uint32_t param) {
    (void)t;
    if (!param) return SCE_COMMON_DIALOG_ERROR_NULL;
    if (!vita_mapped(p, param, SAVE_PARAM_SIZE)) return SAVE_ERROR_PARAM;
    if (p->dialog.kind) return SCE_COMMON_DIALOG_ERROR_BUSY;
    p->dialog.kind = DIALOG_SAVE_DATA, p->dialog.status = STATUS_RUNNING, p->dialog.result = RESULT_OK;
    p->dialog.substatus = STATUS_FINISHED;
    p->dialog.mode = vita_read32(p, param + SAVE_PARAM_MODE);
    p->dialog.slot = save_slot_of(p, param);
    p->dialog.userdata = vita_read32(p, param + SAVE_PARAM_USER);
    return 0;
}
uint32_t vita_dialog_save_data_status(VitaProcess* p, VitaThread* t) {
    (void)t;
    return status(p, DIALOG_SAVE_DATA);
}
uint32_t vita_dialog_save_data_substatus(VitaProcess* p, VitaThread* t) {
    (void)t;
    return p->dialog.kind == DIALOG_SAVE_DATA && p->dialog.status == STATUS_RUNNING ? p->dialog.substatus : STATUS_NONE;
}
/* sceSaveDataDialogContinue(const SceSaveDataDialogParam*): the next screen, shown and answered */
uint32_t vita_dialog_save_data_continue(VitaProcess* p, VitaThread* t, uint32_t param) {
    (void)t;
    if (!param) return SCE_COMMON_DIALOG_ERROR_NULL;
    if (p->dialog.kind != DIALOG_SAVE_DATA || p->dialog.status != STATUS_RUNNING)
        return SCE_COMMON_DIALOG_ERROR_NOT_RUNNING;
    if (!vita_mapped(p, param, SAVE_PARAM_SIZE)) return SAVE_ERROR_PARAM;
    p->dialog.mode = vita_read32(p, param + SAVE_PARAM_MODE);
    if (save_slot_of(p, param)) p->dialog.slot = save_slot_of(p, param);
    p->dialog.substatus = STATUS_FINISHED;
    return 0;
}
/* sceSaveDataDialogFinish(const SceSaveDataDialogFinishParam*) */
uint32_t vita_dialog_save_data_finish(VitaProcess* p, VitaThread* t, uint32_t param) {
    (void)t;
    if (!param) return SCE_COMMON_DIALOG_ERROR_NULL;
    if (p->dialog.kind != DIALOG_SAVE_DATA || p->dialog.status != STATUS_RUNNING)
        return SCE_COMMON_DIALOG_ERROR_NOT_RUNNING;
    if (p->dialog.substatus != STATUS_FINISHED) return SCE_COMMON_DIALOG_ERROR_NOT_FINISHED;
    p->dialog.status = STATUS_FINISHED, p->dialog.substatus = STATUS_RUNNING;
    return 0;
}
uint32_t vita_dialog_save_data_result(VitaProcess* p, VitaThread* t, uint32_t out) {
    (void)t;
    if (p->dialog.kind != DIALOG_SAVE_DATA) return SCE_COMMON_DIALOG_ERROR_NOT_FINISHED;
    if (!out) return SCE_COMMON_DIALOG_ERROR_NULL;
    if (!vita_mapped(p, out, 0x38)) return SAVE_ERROR_PARAM;
    if (p->dialog.status != STATUS_FINISHED && p->dialog.substatus != STATUS_FINISHED)
        return SCE_COMMON_DIALOG_ERROR_NOT_FINISHED;
    vita_write32(p, out, p->dialog.mode);
    vita_write32(p, out + 4, p->dialog.result);
    vita_write32(p, out + 8, p->dialog.result == RESULT_OK ? SAVE_BUTTON_YES : 0);
    vita_write32(p, out + 12, p->dialog.slot);
    uint32_t info = vita_read32(p, out + 16);
    if (info && vita_mapped(p, info, 8)) {
        vita_write32(p, info, vita_savedata_slot_exists(p, p->dialog.slot));
        vita_write32(p, info + 4, 0);
    }
    vita_write32(p, out + 20, p->dialog.userdata);
    return 0;
}
uint32_t vita_dialog_save_data_abort(VitaProcess* p, VitaThread* t) {
    (void)t;
    return abort_dialog(p, DIALOG_SAVE_DATA);
}
uint32_t vita_dialog_save_data_term(VitaProcess* p, VitaThread* t) {
    (void)t;
    if (p->dialog.kind != DIALOG_SAVE_DATA) return SCE_COMMON_DIALOG_ERROR_NOT_IN_USE;
    if (p->dialog.status != STATUS_FINISHED) return SCE_COMMON_DIALOG_ERROR_NOT_FINISHED;
    p->dialog.kind = DIALOG_NONE, p->dialog.status = STATUS_NONE;
    return 0;
}
/* the progress bar's value calls: nothing is drawn */
uint32_t vita_dialog_save_data_progress(VitaProcess* p, VitaThread* t) {
    (void)t;
    return p->dialog.kind == DIALOG_SAVE_DATA ? 0 : SCE_COMMON_DIALOG_ERROR_NOT_IN_USE;
}
uint32_t vita_dialog_zero(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 0;
}
