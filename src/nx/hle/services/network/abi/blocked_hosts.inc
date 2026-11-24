/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   From Eden's blockedDomains (src/core/hle/service/sockets/sfdnsres.cpp, branch lizzie/refactor-networking-12):
     SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project, GPL-3.0-or-later */
/* BLOCKED_HOST(domain): names sfdnsres doesn't resolve, the domain and its subdomains. They are the console's servers
   and services that ban or break on an emulated console: the game gets EAI_AGAIN, as on a console that can't
   reach them. Eden matches any name containing one, here only the domain itself and names under it */
BLOCKED_HOST("nintendo.net") /* Eden: srv.nintendo.net and sun.hac.lp1.d4c.nintendo.net */
BLOCKED_HOST("nintendo.es")
BLOCKED_HOST("nintendowifi.net")
BLOCKED_HOST("nintendo-europe.com")
BLOCKED_HOST("nintendo.com.hk")
BLOCKED_HOST("nintendo.com.au")
BLOCKED_HOST("nintendo.co.kr")
BLOCKED_HOST("nintendo.co.uk")
BLOCKED_HOST("nintendo.co.jp")
BLOCKED_HOST("nintendo.co.nz")
BLOCKED_HOST("nintendo.co.za")
BLOCKED_HOST("nintendo.com")
BLOCKED_HOST("nintendo.jp")
BLOCKED_HOST("nintendo.tw")
BLOCKED_HOST("nintendo.at")
BLOCKED_HOST("nintendo.be")
BLOCKED_HOST("nintendo.dk")
BLOCKED_HOST("nintendo.de")
BLOCKED_HOST("nintendo.fi")
BLOCKED_HOST("nintendo.fr")
BLOCKED_HOST("nintendo.gr")
BLOCKED_HOST("nintendo.hu")
BLOCKED_HOST("nintendo.it")
BLOCKED_HOST("nintendo.nl")
BLOCKED_HOST("nintendo.no")
BLOCKED_HOST("nintendo.pt")
BLOCKED_HOST("nintendo.ru")
BLOCKED_HOST("nintendo.ch")
BLOCKED_HOST("nintendo.se")
BLOCKED_HOST("nintendoswitch.com.cn")
BLOCKED_HOST("nintendoswitch.com")
BLOCKED_HOST("phoenix-api.wbagora.com") /* one game's */
BLOCKED_HOST("battle.net")
BLOCKED_HOST("microsoft.com") /* a game's and others' */
BLOCKED_HOST("mojang.com")
BLOCKED_HOST("xboxlive.com")
BLOCKED_HOST("api.epicgames.dev") /* one game's */
BLOCKED_HOST("minecraftservices.com")
BLOCKED_HOST("508223012e5a5ff19f30a391b2bdadc0.my.2k.com") /* one game's */
#undef BLOCKED_HOST
