/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Switch's language (settings.txt's language, 0 Japanese to 17 Portuguese (Brazil)) as each console's system
   gives its games one: the 3DS's config (cfg), the Vita's system parameter and the DS's firmware user settings */
#ifndef SNX_CONSOLE_LANGUAGE_H
#define SNX_CONSOLE_LANGUAGE_H

#define SNX_LANGUAGES 18 /* the Switch's */
/* the Switch's languages' names, as its menus have them */
static inline const char* snx_language_name(int language) {
    static const char* const names[SNX_LANGUAGES] = {"Japanese",
                                                     "English (US)",
                                                     "French",
                                                     "German",
                                                     "Italian",
                                                     "Spanish",
                                                     "Chinese",
                                                     "Korean",
                                                     "Dutch",
                                                     "Portuguese",
                                                     "Russian",
                                                     "Taiwanese",
                                                     "English (UK)",
                                                     "French (Canada)",
                                                     "Spanish (Latin America)",
                                                     "Chinese (simplified)",
                                                     "Chinese (traditional)",
                                                     "Portuguese (Brazil)"};
    return language >= 0 && language < SNX_LANGUAGES ? names[language] : "?";
}
/* the 3DS's language code (0 Japanese, 1 English, 2 French, 3 German, 4 Italian, 5 Spanish, 6 simplified Chinese,
   7 Korean, 8 Dutch, 9 Portuguese, 10 Russian, 11 traditional Chinese): each of the Switch's has one */
static inline int snx_language_ctr(int language) {
    static const signed char codes[SNX_LANGUAGES] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 1, 2, 5, 6, 11, 9};
    return language >= 0 && language < SNX_LANGUAGES ? codes[language] : 1;
}
/* the Vita's SCE_SYSTEM_PARAM_LANG (0 Japanese, 1 English (US), 2 French, 3 Spanish, 4 German, 5 Italian, 6 Dutch,
   7 Portuguese, 8 Russian, 9 Korean, 10 traditional Chinese, 11 simplified Chinese, 17 Portuguese (Brazil), 18
   English (UK)): each of the Switch's has one */
static inline int snx_language_vita(int language) {
    static const signed char codes[SNX_LANGUAGES] = {0, 1, 2, 4, 5, 3, 11, 9, 6, 7, 8, 10, 18, 2, 3, 11, 10, 17};
    return language >= 0 && language < SNX_LANGUAGES ? codes[language] : 1;
}
/* the DS firmware's (0 Japanese, 1 English, 2 French, 3 German, 4 Italian, 5 Spanish, 6 Chinese): -1 for one of the
   Switch's it has none of (Korean, Dutch, Portuguese, Russian, Taiwanese) */
static inline int snx_language_nds(int language) {
    static const signed char codes[SNX_LANGUAGES] = {0, 1, 2, 3, 4, 5, 6, -1, -1, -1, -1, -1, 1, 2, 5, 6, 6, -1};
    return language >= 0 && language < SNX_LANGUAGES ? codes[language] : -1;
}
#endif
