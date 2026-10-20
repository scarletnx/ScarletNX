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

    def __init__(self, test, body, code='pt_BR', name='Portugu\u00eas (Brasil)', header=None):
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

    def test_the_pot_is_the_code(self):
        self.assertEqual((ROOT / bl.POT).read_text(), bl.pot_text(bl.code_phrases(ROOT)))


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
        self.refused('Cannot open %s: %s', 'N\u00e3o abre %2$s: %1$s')  # numbered: C90's printf has none
        self.refused('Cannot open %s: %s', 'N\u00e3o abre %s: %s%n')

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
        self.assertEqual((code, name, stale), ('pt_BR', 'Portugu\u00eas (Brasil)', ['Gone from the code']))
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
            entry('', 'Language: pt_BR\\n'),
            entry('Cannot open %s: %s', 'N\u00e3o abre %s'),
        ]
        for body in bodies:
            with self.assertRaises(bl.Refused, msg=body):
                self.compile(body)

    def test_letters_the_font_draws(self):
        # CP437's, and the Latin letters past it: the a and o with a tilde, the s with a comma
        self.compile(entry('Settings', 'Configura\u00e7\u00f5es') + entry('Back', 'N\u00e3o') + entry('Up', '\u0219'))
        for text in ('Ajustes\u2026', '\u201cAjustes\u201d', '\u8a2d\u5b9a', 'Ajustes \U0001f600'):
            with self.assertRaises(bl.Refused, msg=text):
                self.compile(entry('Settings', text))
        with self.assertRaises(bl.Refused):  # the name Settings > App > Language shows
            self.compile(entry('Settings', 'Ajustes'), name='\u65e5\u672c\u8a9e')
        self.assertIn(0x00E3, DRAWN)
        self.assertIn(0x00E9, DRAWN)
        self.assertNotIn(0x2026, DRAWN)
        # a wide letter a language shows (tests/po/xx.po's), kept by scripts/build-glyphs.py; one none shows is refused,
        # saying how it gets its glyph
        self.compile(entry('Settings', '\u83dc\u5355'))
        self.assertIn(0x83DC, DRAWN)
        self.assertNotIn(0x9F8D, DRAWN)
        with self.assertRaisesRegex(bl.Refused, r'\(U\+9F8D\).*build-glyphs\.py --from'):
            self.compile(entry('Settings', '\u83dc\u9f8d'))

    def test_refused_headers_and_names(self):
        with self.assertRaises(bl.Refused):
            self.compile(entry('Settings', 'Ajustes'), header='')
        with self.assertRaises(bl.Refused):
            self.compile(entry('Settings', 'Ajustes'), header=HEADER.format(code='pt_PT', name='Portugu\u00eas'))
        with self.assertRaises(bl.Refused):
            self.compile(entry('Settings', 'Ajustes'), header=HEADER.format(code='pt_BR', name=''))
        with self.assertRaises(bl.Refused):
            self.compile(
                entry('Settings', 'Ajustes'),
                header=HEADER.format(code='pt_BR', name='x').replace('charset=UTF-8', 'charset=ISO-8859-1'),
            )
        for code in ('en', 'PT_br', 'portuguese', 'pt-BR'):
            with self.assertRaises(bl.Refused, msg=code):
                self.compile(entry('Settings', 'Ajustes'), code=code)
        with self.assertRaises(bl.Refused):
            self.compile(entry('Settings', 'Configura\u00e7\u00f5es').encode('latin-1'))

    def test_the_c_source(self):
        mo = bl.mo_bytes({'': 'h', 'a': 'b'})
        text = bl.c_text([('pt_BR', 'Portugu\u00eas "?"', mo, [])], 'po/')
        self.assertIn('{"en", "English", NULL, 0},', text)
        self.assertIn('{"pt_BR", "Portugu\\303\\252s \\042\\077\\042", mo_pt_BR, sizeof mo_pt_BR},', text)
        self.assertIn('const int snx_gui_language_count = 2;', text)
        self.assertTrue(text.isascii())


if __name__ == '__main__':
    unittest.main()
