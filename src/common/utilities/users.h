/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The console's users, which the runtime's acc lists, am selects and fs keeps the saves of, and the launcher shows at
   the right of its head: users/users.ini of a data folder, up to eight, each a [userN] section in the console's order,
   and their pictures beside it as users/<uid>.jpg (the bytes the account service keeps, 128 KiB at most). User 1 has the uid users.ini gives it; with none (no users.ini, or
   a [user1] without one) the uid ScarletNX always gave it (SNX_USERS_FIRST_UID0 and 1), so the saves made before there
   were others stay its own. Without users.ini it is the only user, "Player". A nickname is what the console's account
   service keeps (SwitchBrew's ProfileBase): 1 to 32 bytes of UTF-8, with no control character, and no space at
   either end, which users.ini would not keep. Anything users.ini says that is not a user is refused */
#ifndef SNX_COMMON_USERS_H
#define SNX_COMMON_USERS_H
#include <stddef.h>
#include <stdint.h>

#define SNX_USERS_MAX          8  /* the console's users, at most (its limit) */
#define SNX_USERS_NICKNAME     32 /* nn::account::Nickname's bytes before its NUL */
#define SNX_USERS_NICKNAME_MIN 1  /* a nickname's bytes, as the account service keeps it */
#define SNX_USERS_NICKNAME_MAX 32
#define SNX_USERS_FIRST_UID0   0x0123456789ABCDEFull
#define SNX_USERS_FIRST_UID1   0xFEDCBA9876543210ull
typedef struct {
    uint64_t uid[2]; /* nn::account::Uid's two halves */
    char nickname[SNX_USERS_NICKNAME + 1];
    uint32_t picture_at, picture_size; /* users/<uid>.jpg, in users.c's one block of pictures */
} SnxUser;
extern SnxUser g_users[SNX_USERS_MAX]; /* user 1 first, g_user_count of them */
extern int g_user_count;

/* the users of users.ini in dir (one, user 1, without it), the one playing selected (1-based): 0, or -1 with why the
   file or selected is no list of users, and user 1 alone */
int snx_users_load(const char* dir, int selected, char* why, size_t why_size);
/* the users/ folder of the data folder that folder is in (snx_menu_dir's), or NULL out of memory */
char* snx_users_dir(const char* folder);
const SnxUser* snx_users_find(const uint64_t uid[2]);                        /* NULL for no user */
const SnxUser* snx_users_selected(void);                                     /* the user playing (the .ini's user) */
int snx_users_selected_number(void);                                         /* and its number, 1 to g_user_count */
const unsigned char* snx_users_picture(const SnxUser* user, uint32_t* size); /* the JPEG, or NULL and 0 */
/* 1 when nickname follows the rule: SNX_USERS_NICKNAME_MIN to _MAX bytes of UTF-8 (no overlong form, no surrogate, up
   to U+10FFFF), no control character (C0, DEL, C1), and no space at either end */
int snx_users_nickname_valid(const char* nickname);
/* the nickname an Eden room knows user by: its own, which a room whose rule refuses it says (net_room.c) */
const char* snx_users_room_nickname(const SnxUser* user);
/* uid as 32 hex digits, uid[0] first: users.ini's, the pictures' and the saves' */
void snx_users_uid_text(const uint64_t uid[2], char text[33]);
/* users.ini in dir started over with one user, user 1 of uid (not zero) and nickname (the rule's), the one there before
   kept as users.ini.old, which replaces the one an earlier start left. 0, else -1 with why: nothing changed, unless why
   says users.ini is gone and its users are in users.ini.old */
int snx_users_start(const char* dir, const uint64_t uid[2], const char* nickname, char* why, size_t why_size);
/* user number's (1-based, of those loaded) nickname in users.ini of dir made nickname (the rule's), the file's other
   lines as they were; without users.ini user 1's starts it (snx_users_start). 0, else -1 with why: nothing changed */
int snx_users_rename(const char* dir, int number, const char* nickname, char* why, size_t why_size);
#endif
