# YTC UI translations

Each `<code>.json` is one language: a flat map of **string key → translated text**.
`languages.json` lists the languages shown in Settings (order matters) and maps each
to its Innertube `hl`/`gl` (which makes YouTube return localized titles/metadata).

These files are loaded at runtime — **edit them and relaunch, no rebuild needed.**

## Edit a translation
Open the language file (e.g. `es.json`) and change the value. Keep the key. UTF-8.
Anything missing falls back to English, so partial files are fine.

## Add a language
1. Copy `en.json` to `<code>.json` and translate the values.
2. Add an entry to `languages.json`:
   `{ "code": "<code>", "name": "<endonym>", "hl": "<yt-lang>", "gl": "<yt-country>" }`
3. The bundled font (DejaVuSans) covers Latin, Cyrillic, and Greek. Other scripts
   (CJK, Arabic, Indic) need a different font and, for some, text shaping.

## Keys
The keys are fixed identifiers used in the code (`i18n::Str`). English is also baked
into the binary as the ultimate fallback, so the app still works if these files are
missing. To regenerate all files from the built-in English defaults + current values:
`g++ -std=c++17 tools/dump_i18n.cpp src/i18n.cpp -o /tmp/dump_i18n && /tmp/dump_i18n data/lang`
(only needed by developers when adding new string keys).
