# Korean support

Select **Settings → Language → 한국어**. The UI uses Korean translations and
YouTube's `ko`/`KR` locale. The original five-row keyboard keeps its row spans,
number row, spatial navigation, wide space bar and submit key. Latin letter
positions map to standard two-set Hangul. Shift is one-shot, including doubled
consonants and ㅒ/ㅖ. The former underscore key toggles Korean/English; Shift+hyphen
still enters an underscore. Numeric TV-code entry retains its layout.

Composition supports compound vowels/finals and moving a final consonant into
the next syllable. Backspace undoes jamo steps while composition is active;
committed text is removed one Unicode codepoint at a time. Cursor movement,
spaces, punctuation, language toggles, external text input and submission commit
the active composition. Clearing or reopening the keyboard resets it.

## Font

`data/fonts/NotoSansKR-Regular.ttf` is a static weight-400 instance of Google
Fonts' Noto Sans KR variable font, licensed under SIL OFL 1.1. The OFL notice is
beside the font. Existing DejaVu and Japanese fallback assets remain available.
The same font and language assets are mirrored in the PortMaster staging tree.
No executable or media library changes are needed in that tree.

To reproduce both font copies:

```sh
python3 -m pip install fonttools==4.61.1
python3 tools/make_korean_font.py
```

The script pins the source repository revision and verifies its SHA-256, disables
font timestamp recalculation and verifies all 11,172 modern Hangul syllables plus
compatibility jamo. An offline `--source` argument accepts the identical variable
font and performs the same checksum check.

## Tests

```sh
c++ -std=c++17 -Isrc tools/test_hangul.cpp -o /tmp/ytc-test-hangul
/tmp/ytc-test-hangul
```

Tests cover complex vowels/finals, final migration, per-key undo, insertion into
existing text, Unicode cursor boundaries and round trips of all 11,172 syllables.
The corresponding application changes were also checked in a desktop/offscreen
640×480 integration run (actual key activation, Korean/English, Shift, deletion,
mid-text insertion, clear, long input, numeric keypad and Settings rendering).
The contributor confirmed the test package launches on a Retroid Pocket Duo Lite
in a PortMaster environment. This does not assert separate verification of
network search, video playback or every firmware/device combination.

## Keyboard references

The two-set layout and composition work used the supplied
`spruceOS-Korean-Keyboard-test-v0.1` reference (`hangul_input.py`), which in turn
identifies `bulzipke/batocera-emulationstation`'s `knulli_korean_keyboard` branch
(`StringUtil.cpp` / `GuiTextEditPopupKeyboard.cpp`) as its reference. The C++ editor
adds YTC-specific byte-safe cursor handling and reversible edits around that
composition behavior. This PR does not import the spruce UI or its theme.

The source archive's spruceUI notice is retained at
`licenses/spruce-keyboard-reference.txt`, and its EmulationStation MIT notice
(Copyright 2014 Alec Lofquist) at `licenses/keyboard-reference-MIT.txt`.
The font's separate OFL notice is `data/fonts/OFL-NotoSansKR.txt`.
