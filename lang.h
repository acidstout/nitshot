/*
 * lang.h - which language the interface speaks, and where the words come from.
 *
 * English is compiled in and is always the fallback, per string rather than
 * per file: a language file that is missing, damaged or simply incomplete
 * degrades to English word by word instead of leaving blank buttons.
 *
 * Everything else lives in `langs\<code>.ini` beside the exe, one file per
 * language, and a user can add one without a rebuild - drop `fr.ini` in and it
 * appears in the Settings list. `%APPDATA%\Nitshot\langs\` is
 * searched first, so a translation can be replaced on an installation whose
 * program folder is read-only.
 *
 * The files are UTF-8 and are parsed here rather than through
 * GetPrivateProfileString, which reads a BOM-less file in the system code page
 * and would turn every umlaut into mojibake the moment someone saved from
 * Notepad.
 */
#ifndef NITSHOT_LANG_H
#define NITSHOT_LANG_H

#include "nitshot.h"
#include "strings.h"

/*
 * Picks the language and loads it. 'code' is a two-letter tag, or empty/NULL
 * for "follow Windows" via GetUserDefaultUILanguage.
 */
void Lang_Init(const wchar_t *code);

/* The tag actually in use ("en", "de", ...), never NULL. */
const wchar_t *Lang_Current(void);

/* Never NULL, never empty: falls back to the built-in English. */
const wchar_t *Lang_Str(StringId id);

/*
 * Substitutes %1..%9 with the wide strings that follow, in whatever order the
 * translation puts them. Returns 'buf'.
 *
 * This is deliberately not FormatMessage: that treats %n as a hard line break
 * and %.  %! as escapes, all of which a translator would eventually trip over
 * by accident, and it would make a user-supplied file able to crash the
 * formatter rather than merely look wrong.
 */
const wchar_t *Lang_Format(wchar_t *buf, size_t cch, StringId id, ...);

/*
 * The language tags that have a file, for the Settings list. Returns how many
 * were written into 'codes'.
 */
int Lang_List(wchar_t codes[][8], int max);

/* Frees the loaded table. */
void Lang_Shutdown(void);

#endif /* NITSHOT_LANG_H */
