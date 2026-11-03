# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""scripts/build-languages.py: the phrases it finds in the code, the .po files it refuses, and the catalogs it makes."""

import gettext
import importlib.util
import io
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('build_languages', ROOT / 'scripts/build-languages.py')
bl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bl)

HEADER = """msgid ""
msgstr ""
"Language: {code}\\n"
"Content-Type: text/plain; charset=UTF-8\\n"
"X-Language-Name: {name}\\n"

"""
# the phrases a .po translates here, as the .pot lists them
PHRASES = {
    'Settings': (set(), []),
    'Page %zu/%zu, %zu game(s)': (set(), []),
    'Cannot open %s: %s': (set(), []),
    '%.*s of %d': (set(), []),
    'Up/Down: row. Enter: open. ESC: back.': ({'snx-keys'}, []),
    'Arrows/mouse: move. Back: go back.': ({'snx-keys'}, []),
    'ESC': ({'snx-key'}, []),
    'Back': ({'snx-key'}, []),
    'Up': ({'snx-key'}, []),
    'Down': ({'snx-key'}, []),
}


DRAWN = bl.drawable(ROOT)


class Po:
    """a .po written in a folder of its own"""

    def __init__(self, test, body, code='pt-BR', name='Portugu\u00eas (Brasil)', header=None):
        self.folder = tempfile.TemporaryDirectory(prefix='snx-languages-')
        test.addCleanup(self.folder.cleanup)
        self.path = Path(self.folder.name) / f'{code}.po'
        head = (HEADER.format(code=code, name=name) if header is None else header).encode()
        self.path.write_bytes(head + (body if isinstance(body, bytes) else body.encode()))


def entry(msgid, msgstr):
    return f'msgid {bl.po_quote(msgid)}\nmsgstr {bl.po_quote(msgstr)}\n\n'


class Phrases(unittest.TestCase):
    def test_marks_literals_and_their_pieces(self):
        source = r"""
/* snx_tr("a comment's") */
x = snx_tr("Hello, " "world\n"); // snx_tr("a line comment's")
y = snx_tr(app->message);
char c = ')', d = '\'', e = '\012';
#define SAY SNX_TR_KEYS("Up/Down: move. "  \
                        "Enter: open.")
static const char* k[] = {SNX_TR_KEY("ESC"), SNX_TR_NOOP("A \"quoted\" (paren)")};
"""
        found = [(p, flag) for p, flag, _ in bl.phrases_of(source, 't.c')]
        self.assertEqual(
            found,
            [
                ('Hello, world\n', None),
                ('Up/Down: move. Enter: open.', 'snx-keys'),
                ('ESC', 'snx-key'),
                ('A "quoted" (paren)', None),
            ],
        )

    def test_refuses_a_phrase_it_cannot_list(self):
        for source in ('snx_tr(on ? "a" : "b");', 'snx_tr("a" SAY_B);', 'snx_tr(snx_tr("a"));', 'snx_tr("a"'):
            with self.assertRaises(bl.Refused, msg=source):
                bl.phrases_of(source, 't.c')

    def test_the_texts_of_the_scenes(self):
        # ui.txt's items: [type]"Text", @ or not, its escapes taken; names, properties and comments are no phrase
        text = '# [button]"a comment"\n[scene]S {\n\t[list]L {\n\t\t[button]"Load \\"A\\" \\\\ B" {}\n'
        text += '\t\t@[button]"Decor" {\n\t\t\tkey = d\n\t\t}\n\t}\n\t[label]Status {}\n}\n'
        self.assertEqual(bl.ui_phrases(text), ['Load "A" \\ B', 'Decor'])
        self.assertEqual(bl.code_phrases(ROOT)['Load Switch NSP/XCI/NRO'][1], [bl.UI])

    def test_the_pot_is_the_code(self):
        self.assertEqual((ROOT / bl.POT).read_text(), bl.pot_text(bl.code_phrases(ROOT)))

    def test_the_setup_notices_the_menu_shows(self):
        # setup.c's marked words, the setup's pages showing them; its paths and options left as they are
        phrases = bl.code_phrases(ROOT)
        self.assertEqual(phrases['Account saves'][1], ['src/common/utilities/setup.c'])
        self.assertIn("the user's data folder", phrases)
        self.assertNotIn('<id>_data/save/user', phrases)
        self.assertNotIn('--keep-c', phrases)


class Formats(unittest.TestCase):
    def ok(self, msgid, msgstr, keys=False):
        bl.check_entry(msgid, msgstr, keys, {w: w for w in ('ESC', 'Back', 'Up', 'Down')}, 'x')

    def refused(self, msgid, msgstr, keys=False):
        with self.assertRaises(bl.Refused, msg=msgstr):
            self.ok(msgid, msgstr, keys)

    def test_arguments(self):
        self.assertEqual(
            bl.conversions('%-10s %.*s %zu %ld %3d%% %c %p %g', 'x'),
            [' string', 'int', ' string', 'z unsigned', 'l signed', ' signed', ' char', ' pointer', ' double'],
        )
        self.ok('Cannot open %s: %s', 'N\u00e3o abre %s: %s')
        self.ok('Page %zu/%zu, %zu game(s)', 'P\u00e1gina %zu/%zu, %zu jogo(s)')
        self.ok('%.*s of %d', '%.*s de %i')  # the same type, another letter
        self.ok('%-10s', '%-12s')  # a width of its own
        self.refused('Cannot open %s: %s', 'N\u00e3o abre: %s')  # one missing
        self.refused('Cannot open %s: %s', '%s %s %s')  # one more
        self.refused('%.*s of %d', '%d de %.*s')  # in another order
        self.refused('Page %zu/%zu, %zu game(s)', 'P\u00e1gina %d/%d, %d jogo(s)')  # another type
        self.refused('%.*s of %d', '%s de %d')  # the precision's int gone
        self.refused('Cannot open %s: %s', 'N\u00e3o abre %2$s: %1$s')  # numbered: ISO C's printf has none
        self.refused('Cannot open %s: %s', 'N\u00e3o abre %s: %s%n')

    def test_named_placeholders(self):
        self.ok('{count} games in {folder}', '{folder}: {count} jogos')  # by name, so in any order
        self.refused('{count} games', '{n} jogos')
        self.refused('{count} games', 'jogos')
        self.refused('Settings', 'Ajustes {x}')

    def test_a_percent_the_english_does_not_have(self):
        self.refused('Settings', 'Ajustes %s')
        self.refused('Settings', 'Ajustes %d')
        self.refused('Settings', '100% Ajustes')
        # none of them a conversion, which printf would still read through a phrase it takes as a format
        self.refused('Settings', 'Ajustes 100%')
        self.refused('Settings', 'Ajustes: 50%!')

    def test_keys(self):
        english = 'Up/Down: row. Enter: open. ESC: back.'
        self.ok(english, 'Up/Down: linha. Enter: abre. ESC: volta.', True)
        self.ok(english, 'Up/Down: linha. Enter: abre. Sem tecla aqui. ESC: volta.', True)  # a sentence of no key
        self.refused(english, 'Cima/Baixo: linha. Enter: abre. ESC: volta.', True)
        self.refused(english, 'Up/Down: linha. Enter: abre. Esc: volta.', True)
        self.refused(english, 'Up/Down: linha. ESC: volta.', True)
        self.refused(english, 'Up: linha. Enter: abre. ESC: volta.', True)
        self.ok(english, 'Up/Down: linha. Enter: abre. ESC: volta.', False)
        # a word of no key's is the translator's
        self.ok('Arrows/mouse: move. Back: go back.', 'Setas/mouse: move. Back: volta.', True)

    def test_greyed_sentences(self):
        english = 'Enter: pick. ~Del: take one off. ESC: back.'
        self.assertEqual(bl.sentences_keys(english), [(False, ['Enter']), (True, ['Del']), (False, ['ESC'])])
        self.ok(english, 'Enter: escolhe. ~Del: tira uma. ESC: volta.', True)
        self.refused(english, 'Enter: escolhe. Del: tira uma. ESC: volta.', True)  # no longer greyed: Del pressed
        self.refused(english, '~Enter: escolhe. Del: tira uma. ESC: volta.', True)  # another greyed
        tap = 'Tap: pick. ~Tap a folder twice to take it off. Back: go back.'
        self.ok(tap, 'Tap: escolhe. ~Toque duas vezes numa pasta para tira-la. Back: volta.', True)
        self.refused(tap, 'Tap: escolhe. Toque duas vezes numa pasta para tira-la. Back: volta.', True)


class Files(unittest.TestCase):
    def compile(self, body, **kw):
        return bl.compile_po(Po(self, body, **kw).path, PHRASES, DRAWN)

    def test_a_catalog_gettext_reads(self):
        code, name, mo, stale = self.compile(
            entry('Settings', 'Configura\u00e7\u00f5es')
            + entry('Cannot open %s: %s', 'N\u00e3o abre %s: %s')
            + '#, fuzzy\n'
            + entry('Back', 'Volta')
            + entry('Up', '')
            + entry('Gone from the code', 'Sumiu')
            + '#~ msgid "Older"\n#~ msgstr "Antigo"\n\n'
            + '# its comment\n#~ msgid "Oldest"\n#~ msgstr "O mais antigo"\n'
        )
        self.assertEqual((code, name, stale), ('pt-BR', 'Portugu\u00eas (Brasil)', ['Gone from the code']))
        catalog = gettext.GNUTranslations(io.BytesIO(mo))
        self.assertEqual(catalog.gettext('Settings'), 'Configura\u00e7\u00f5es')
        self.assertEqual(catalog.gettext('Cannot open %s: %s'), 'N\u00e3o abre %s: %s')
        self.assertEqual(catalog.gettext('Back'), 'Back')  # fuzzy: not taken
        self.assertEqual(catalog.gettext('Up'), 'Up')  # not translated
        self.assertEqual(catalog.gettext('Older'), 'Older')

    def test_sorted_by_bytes(self):
        mo = bl.mo_bytes({'': 'Content-Type: text/plain; charset=UTF-8\n', 'b': '1', '\u00e1': '2', 'B': '3', 'a': '4'})
        catalog = gettext.GNUTranslations(io.BytesIO(mo))
        self.assertEqual([catalog.gettext(k) for k in ('a', 'B', 'b', '\u00e1')], ['4', '3', '1', '2'])
        n, originals = int.from_bytes(mo[8:12], 'little'), int.from_bytes(mo[12:16], 'little')
        keys = []
        for i in range(n):
            length = int.from_bytes(mo[originals + 8 * i : originals + 8 * i + 4], 'little')
            at = int.from_bytes(mo[originals + 8 * i + 4 : originals + 8 * i + 8], 'little')
            keys.append(mo[at : at + length])
        self.assertEqual(keys, sorted(keys))

    def test_keys_kept_or_translated_here(self):
        english = 'Up/Down: row. Enter: open. ESC: back.'
        self.compile(entry('ESC', 'Sair') + entry(english, 'Up/Down: linha. Enter: abre. Sair: volta.'))
        self.compile(entry(english, 'Up/Down: linha. Enter: abre. ESC: volta.'))
        with self.assertRaises(bl.Refused):  # a translation of the key, not this file's
            self.compile(entry('ESC', 'Sair') + entry(english, 'Up/Down: linha. Enter: abre. Esc: volta.'))
        with self.assertRaises(bl.Refused):  # a key this file does not translate
            self.compile(entry(english, 'Up/Down: linha. Enter: abre. Sair: volta.'))

    def test_refused(self):
        bodies = [
            'msgctxt "menu"\n' + entry('Settings', 'Ajustes'),
            'msgid "Settings"\nmsgid_plural "Settings"\nmsgstr[0] "Ajustes"\n\n',
            'msgid "Settings"\nmsgstr[0] "Ajustes"\n\n',
            entry('Settings', 'Ajustes') + entry('Settings', 'Configura\u00e7\u00f5es'),
            'msgid "Settings"\n# a comment\nmsgstr "Ajustes"\n\n',
            'msgid "Settings"\nmsgstr "Aj\\x41stes"\n\n',
            'msgid "Settings"\nmsgstr "Aj"ustes"\n\n',
            'msgid "Settings"\nmsgstr Ajustes\n\n',
            'msgstr "Ajustes"\n\n',
            'msgid "Settings"\n\n',
            'msgid "Settings"\nmsgstr "Ajustes"\nmsgstr "Outra"\n\n',
            'what is this\n\n',
            entry('', 'Language: pt-BR\\n'),
            entry('Cannot open %s: %s', 'N\u00e3o abre %s'),
        ]
        for body in bodies:
            with self.assertRaises(bl.Refused, msg=body):
                self.compile(body)

    def test_letters_the_font_draws(self):
        # CP437's, and the Latin letters past it: the a and o with a tilde, the s with a comma
        self.compile(entry('Settings', 'Configura\u00e7\u00f5es') + entry('Back', 'N\u00e3o') + entry('Up', '\u0219'))
        for text in ('Ajustes\u2026', '\u201cAjustes\u201d', '\ua000\ua001', 'Ajustes \U0001f600'):
            with self.assertRaises(bl.Refused, msg=text):
                self.compile(entry('Settings', text))
        with self.assertRaises(bl.Refused):  # the name Settings > App > Language shows
            self.compile(entry('Settings', 'Ajustes'), name='\ua000\ua001')
        # the titles' blocks, Chinese, Japanese and Korean, whatever the languages show
        self.compile(entry('Settings', '\u8a2d\u5b9a') + entry('Back', '\u3082\u3069\u308b \ub4a4\ub85c'))
        self.compile(entry('Settings', 'Ajustes'), name='\u65e5\u672c\u8a9e')
        self.assertIn(0x00E3, DRAWN)
        self.assertIn(0x00E9, DRAWN)
        self.assertNotIn(0x2026, DRAWN)
        # a wide letter a language shows (tests/po/xx.po's), kept by scripts/build-glyphs.py; one none shows is refused,
        # saying how it gets its glyph
        self.compile(entry('Settings', '\u83dc\u5355'))
        self.assertIn(0x83DC, DRAWN)
        self.assertIn(0x9F8D, DRAWN)  # the titles' blocks, whatever the languages show
        self.assertNotIn(0xA000, DRAWN)
        with self.assertRaisesRegex(bl.Refused, r'\(U\+A000\).*build-glyphs\.py --from'):
            self.compile(entry('Settings', '\u83dc\ua000'))

    def test_refused_headers_and_names(self):
        with self.assertRaises(bl.Refused):
            self.compile(entry('Settings', 'Ajustes'), header='')
        with self.assertRaises(bl.Refused):
            self.compile(entry('Settings', 'Ajustes'), header=HEADER.format(code='pt-PT', name='Portugu\u00eas'))
        with self.assertRaises(bl.Refused):
            self.compile(entry('Settings', 'Ajustes'), header=HEADER.format(code='pt-BR', name=''))
        with self.assertRaises(bl.Refused):
            self.compile(
                entry('Settings', 'Ajustes'),
                header=HEADER.format(code='pt-BR', name='x').replace('charset=UTF-8', 'charset=ISO-8859-1'),
            )
        # a BCP 47 tag as written for it: language, Script, REGION; the locale's forms and English refused
        for code in ('en', 'PT_br', 'portuguese', 'pt_BR', 'pt-br', 'zh_CN', 'zh-hans-CN', 'zh-Hans-cn', 'pt-BRA'):
            with self.assertRaisesRegex(bl.Refused, 'BCP 47 tag', msg=code):
                self.compile(entry('Settings', 'Ajustes'), code=code)
        for code in ('pt-BR', 'pt', 'zh-Hans-CN', 'zh-Hant', 'es-419'):
            self.assertEqual(self.compile(entry('Settings', 'Ajustes'), code=code)[0], code)
        with self.assertRaises(bl.Refused):
            self.compile(entry('Settings', 'Configura\u00e7\u00f5es').encode('latin-1'))

    def test_a_po_is_held_to_the_pot(self):
        # a phrase of the .pot the .po has no entry for, and an entry of a phrase the .pot no longer has, each named
        # with its line; one untranslated or fuzzy is an entry
        phrases = {'Settings': (set(), ['src/gui/a.c']), 'Back': ({'snx-key'}, ['src/gui/a.c'])}
        phrases['Load %s'] = (set(), ['src/gui/b.c'])
        folder = Path(Po(self, '').path).parent / 'po'
        folder.mkdir()
        pot = folder / 'scarletnx.pot'
        pot.write_text(bl.pot_text(phrases))
        po = folder / 'pt-BR.po'
        body = entry('Settings', 'Ajustes') + '#, fuzzy\n' + entry('Back', 'Voltar') + entry('Share log', 'Partilhar')
        po.write_text(HEADER.format(code='pt-BR', name='Português (Brasil)') + body)
        self.assertEqual(
            bl.po_drift(po, pot),
            [
                "po/pt-BR.po: 'Load %s' (po/scarletnx.pot:22) has no entry",
                "po/pt-BR.po:14: 'Share log' is no phrase of po/scarletnx.pot",
            ],
        )
        # --pot's merge: the .pot's order and comments, the fuzzy mark kept, the phrase it lacked untranslated, the one
        # the code dropped obsolete; read again, the same
        merged = bl.merged_po(po, phrases)
        self.assertIn('#: src/gui/a.c\nmsgid "Settings"\nmsgstr "Ajustes"\n', merged)
        self.assertIn('#, fuzzy, snx-key\nmsgid "Back"\nmsgstr "Voltar"\n', merged)
        self.assertIn('#: src/gui/b.c\n#, c-format\nmsgid "Load %s"\nmsgstr ""\n', merged)
        self.assertTrue(merged.endswith('#~ msgid "Share log"\n#~ msgstr "Partilhar"\n'))
        self.assertTrue(merged.startswith(HEADER.format(code='pt-BR', name='Português (Brasil)')))
        # --check only warns of it: the drift and how many entries lag, never a refusal
        warned = bl.lagging(folder, pot, DRAWN)
        self.assertEqual(warned[:2], bl.po_drift(po, pot))
        self.assertIn('2 entries lag po/scarletnx.pot, which the menu shows in English', warned[2])
        po.write_text(merged)
        self.assertEqual(bl.po_drift(po, pot), [])
        self.assertEqual(bl.lagging(folder, pot, DRAWN), [])
        self.assertEqual(bl.merged_po(po, phrases), merged)
        # the phrase back in the code takes its obsolete translation back
        phrases['Share log'] = (set(), ['src/gui/c.c'])
        again = bl.merged_po(po, phrases)
        self.assertIn('msgid "Share log"\nmsgstr "Partilhar"\n', again)
        self.assertNotIn('#~', again)

    def test_the_c_source(self):
        mo = bl.mo_bytes({'': 'h', 'a': 'b'})
        text = bl.c_text([('pt-BR', 'Portugu\u00eas "?"', mo, []), ('zh-Hans-CN', 'x', mo, [])], 'po/')
        self.assertIn('{"en", "English", NULL, 0},', text)
        self.assertIn('{"pt-BR", "Portugu\\303\\252s \\042\\077\\042", mo_pt_BR, sizeof mo_pt_BR},', text)
        self.assertIn('static const unsigned char mo_zh_Hans_CN[] = {', text)
        self.assertIn('{"zh-Hans-CN", "x", mo_zh_Hans_CN, sizeof mo_zh_Hans_CN},', text)
        self.assertIn('const int snx_gui_language_count = 3;', text)
        self.assertTrue(text.isascii())


if __name__ == '__main__':
    unittest.main()
