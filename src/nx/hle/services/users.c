/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The console's users (common/utilities/users.c) of the data folder the game is in, the user's for any other folder:
   the game runs as the one its .ini's `user_uid` names (the deprecated `user`'s number while it names none), and
   anything users.ini says that is not a user stops it */
#include "nx/hle/services/services_int.h"
#include "common/utilities/common.h"
#include "nx/runtime/ini.h"
#include "nx/hle/hle.h"

void snx_users_init(void) {
    char why[512], uid[33], *dir = snx_users_dir(g_exedir[0] ? g_exedir : ".");
    int old;
    if (!dir) fatal("Out of memory finding the users folder");
    if (snx_users_choose(dir, g_settings.user_uid, g_settings.user_uid[0] ? NULL : ini_deprecated_value("user"), &old,
                         why, sizeof why))
        fatal("The console's users: %s", why);
    if (old) { /* the uid, not the nickname: it names no one */
        snx_users_uid_text(snx_users_selected()->uid, uid);
        WARN("settings: user=%d is deprecated: user_uid=%s, [user%d]'s, replaces it (the launcher's Current user "
             "writes it)\n",
             old, uid, old);
    }
    if (strlen(dir) >= sizeof g_host.acc.users_dir) fatal("The users folder %s is a path too long to write in", dir);
    memcpy(g_host.acc.users_dir, dir, strlen(dir) + 1); /* where acc:su writes users.ini */
    /* neither the folder nor the nickname: players attach the log to public reports, and both can name them */
    LOG("acc: %d user%s, playing as user %d\n", g_user_count, g_user_count == 1 ? "" : "s", snx_users_selected_number());
    /* each user's Mii and picture not taken as users.ini says them: what it says and what is in effect */
    if (snx_users_notes()[0]) LOG("%s", snx_users_notes());
    free(dir);
}
