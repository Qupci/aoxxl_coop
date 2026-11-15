// The mod's texts shown to the players (hints, the menus' rows) in the game's
// language, from xxl_coop_lang.ini next to xxl_coop.ini: a [section] per
// language (French, English, German, Spanish, Italian -- the game's packs 00-04
// GLOC), "key = text" lines in UTF-8, "\n" a new line, {name} a placeholder the
// mod fills in (a hero's icon, a button, a number). A key missing from the
// game's language is taken from [English], else shown as the key. The file is
// read again when it changes (edit it with the game running).
//
// The game's language: yellow pages +0x94 -> +0x24 (a short, 0 French .. 4
// Italian), set at boot from the saved options and by the options menu
// (FUN_004090c0, which loads that pack's GLOC).

#pragma once
#include <initializer_list>

struct LocArg { const char* name; const wchar_t* value; };

// `key`'s text in the current language.
const wchar_t* Loc(const char* key);
// `key`'s text with its {name} placeholders filled from `args` (an unknown one
// is left as it is), into `out` (`n` characters).
void LocFormat(wchar_t* out, int n, const char* key, std::initializer_list<LocArg> args = {});
// The language the texts are in: 0 French, 1 English, 2 German, 3 Spanish, 4 Italian.
int LocLanguage();
// Autotests: the texts in language `lang` whatever the game's (-1: the game's
// again). XXL_LANG=n does the same from the start.
void LocForceLanguage(int lang);
