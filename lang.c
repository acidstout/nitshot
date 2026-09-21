/*
 * lang.c - loading language files and looking strings up.
 */

#include "nitshot.h"
#include "lang.h"
#include "log.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <stdarg.h>

/* The built-in English, and the key names that a language file uses. */
static const wchar_t *const g_english[STR_COUNT] = {
#define BSNIP_EN(id, en) L##en,
    BSNIP_STRINGS(BSNIP_EN)
#undef BSNIP_EN
};

/* Stringize, then widen: L#id would paste an L onto the macro body rather
   than onto the string it produces. */
#define BSNIP_WIDE2(x) L##x
#define BSNIP_WIDE(x)  BSNIP_WIDE2(x)

static const wchar_t *const g_keys[STR_COUNT] = {
#define BSNIP_KEY(id, en) BSNIP_WIDE(#id),
    BSNIP_STRINGS(BSNIP_KEY)
#undef BSNIP_KEY
};

/* Loaded overrides; NULL means "use the English above". */
static wchar_t *g_loaded[STR_COUNT];
static wchar_t  g_current[8] = L"en";

const wchar_t *Lang_Current(void)
{
    return g_current;
}

const wchar_t *Lang_Str(StringId id)
{
    if (id < 0 || id >= STR_COUNT)
        return L"";
    return g_loaded[id] ? g_loaded[id] : g_english[id];
}

/* ------------------------------------------------------------- formatting */

const wchar_t *Lang_Format(wchar_t *buf, size_t cch, StringId id, ...)
{
    const wchar_t *src = Lang_Str(id);
    const wchar_t *args[9];
    va_list        ap;
    size_t         out = 0;
    int            i, argc = 0;

    if (!buf || cch == 0)
        return L"";

    /*
     * The argument list is read eagerly and only as far as the highest
     * placeholder the *English* text uses, because a translation that dropped
     * or reordered one must not change how many arguments are consumed.
     */
    for (i = 0; g_english[id][i]; i++)
        if (g_english[id][i] == L'%' &&
            g_english[id][i + 1] >= L'1' && g_english[id][i + 1] <= L'9') {
            int n = g_english[id][i + 1] - L'0';
            if (n > argc) argc = n;
        }

    va_start(ap, id);
    for (i = 0; i < argc && i < 9; i++) {
        args[i] = va_arg(ap, const wchar_t *);
        if (!args[i])
            args[i] = L"";
    }
    va_end(ap);
    for (; i < 9; i++)
        args[i] = L"";

    while (*src && out + 1 < cch) {
        if (src[0] == L'%' && src[1] >= L'1' && src[1] <= L'9') {
            const wchar_t *a = args[src[1] - L'1'];
            while (*a && out + 1 < cch)
                buf[out++] = *a++;
            src += 2;
        } else if (src[0] == L'%' && src[1] == L'%') {
            buf[out++] = L'%';
            src += 2;
        } else {
            buf[out++] = *src++;
        }
    }
    buf[out] = 0;
    return buf;
}

/* ------------------------------------------------------------ the folders */

/* langs\ beside the exe, and the per-user one that overrides it. */
static BOOL LangDir(wchar_t *out, size_t cch, BOOL userDir)
{
    if (userDir) {
        PWSTR appdata = NULL;
        HRESULT hr = SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL,
                                          &appdata);
        if (FAILED(hr))
            return FALSE;
        hr = StringCchPrintfW(out, cch, L"%s\\%s\\langs", appdata, APP_NAME_W);
        CoTaskMemFree(appdata);
        return SUCCEEDED(hr);
    }
    if (!GetModuleFileNameW(NULL, out, (DWORD)cch))
        return FALSE;
    PathRemoveFileSpecW(out);
    return SUCCEEDED(StringCchCatW(out, cch, L"\\langs"));
}

/* ------------------------------------------------------------ the parsing */

/* "\n", "\t" and "\\" are the only escapes; everything else is literal. */
static void Unescape(wchar_t *s)
{
    wchar_t *r = s, *w = s;

    while (*r) {
        if (r[0] == L'\\' && r[1]) {
            switch (r[1]) {
            case L'n': *w++ = L'\n'; r += 2; continue;
            case L't': *w++ = L'\t'; r += 2; continue;
            case L'r': *w++ = L'\r'; r += 2; continue;
            case L'\\': *w++ = L'\\'; r += 2; continue;
            default: break;
            }
        }
        *w++ = *r++;
    }
    *w = 0;
}

static void Trim(wchar_t *s)
{
    wchar_t *end;
    size_t   lead = 0;

    while (s[lead] == L' ' || s[lead] == L'\t')
        lead++;
    if (lead)
        memmove(s, s + lead, (wcslen(s + lead) + 1) * sizeof(wchar_t));

    end = s + wcslen(s);
    while (end > s && (end[-1] == L' ' || end[-1] == L'\t' ||
                       end[-1] == L'\r' || end[-1] == L'\n'))
        *--end = 0;
}

/* Reads a UTF-8 file, with or without a BOM, into freshly allocated wide text. */
static wchar_t *ReadUtf8(const wchar_t *path)
{
    HANDLE   f;
    DWORD    size, got = 0;
    char    *raw;
    wchar_t *text = NULL;
    int      chars;
    DWORD    skip = 0;

    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return NULL;
    size = GetFileSize(f, NULL);
    if (size == INVALID_FILE_SIZE || size > 4u * 1024u * 1024u) {
        CloseHandle(f);
        return NULL;
    }
    raw = (char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)size + 1);
    if (!raw) {
        CloseHandle(f);
        return NULL;
    }
    if (!ReadFile(f, raw, size, &got, NULL) || got != size) {
        HeapFree(GetProcessHeap(), 0, raw);
        CloseHandle(f);
        return NULL;
    }
    CloseHandle(f);
    raw[size] = 0;

    if (size >= 3 && (BYTE)raw[0] == 0xEF && (BYTE)raw[1] == 0xBB &&
        (BYTE)raw[2] == 0xBF)
        skip = 3;

    chars = MultiByteToWideChar(CP_UTF8, 0, raw + skip, (int)(size - skip),
                                NULL, 0);
    if (chars > 0) {
        text = (wchar_t *)HeapAlloc(GetProcessHeap(), 0,
                                    ((SIZE_T)chars + 1) * sizeof(wchar_t));
        if (text) {
            MultiByteToWideChar(CP_UTF8, 0, raw + skip, (int)(size - skip),
                                text, chars);
            text[chars] = 0;
        }
    }
    HeapFree(GetProcessHeap(), 0, raw);
    return text;
}

static void Store(StringId id, const wchar_t *value)
{
    size_t   n = wcslen(value) + 1;
    wchar_t *copy = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, n * sizeof(wchar_t));

    if (!copy)
        return;
    memcpy(copy, value, n * sizeof(wchar_t));
    if (g_loaded[id])
        HeapFree(GetProcessHeap(), 0, g_loaded[id]);
    g_loaded[id] = copy;
}

/* Returns the number of keys understood, or -1 if the file could not be read. */
static int LoadFile(const wchar_t *path)
{
    wchar_t *text = ReadUtf8(path);
    wchar_t *line, *next;
    int      taken = 0, unknown = 0;

    if (!text)
        return -1;

    for (line = text; line && *line; line = next) {
        wchar_t *eq;
        int      i;

        next = wcspbrk(line, L"\r\n");
        if (next) {
            *next = 0;
            next++;
            while (*next == L'\r' || *next == L'\n')
                next++;
        }

        Trim(line);
        if (!*line || *line == L';' || *line == L'#' || *line == L'[')
            continue;

        eq = wcschr(line, L'=');
        if (!eq)
            continue;
        *eq = 0;
        Trim(line);
        Trim(eq + 1);
        Unescape(eq + 1);

        for (i = 0; i < STR_COUNT; i++) {
            if (_wcsicmp(line, g_keys[i]) == 0) {
                /* An empty value means "no translation", not "empty label". */
                if (eq[1])
                    Store((StringId)i, eq + 1);
                taken++;
                break;
            }
        }
        if (i == STR_COUNT)
            unknown++;
    }

    HeapFree(GetProcessHeap(), 0, text);
    if (unknown)
        Log_Printf(L"lang: %s had %d unrecognised key(s), ignored", path, unknown);
    return taken;
}

/* ------------------------------------------------------------ the choosing */

static void CodeFromWindows(wchar_t *out, size_t cch)
{
    LANGID id = GetUserDefaultUILanguage();

    /* The primary language is enough: de-DE, de-AT and de-CH all read de.ini,
       and a translator who needs to separate them can still ship de-AT.ini and
       name it in the setting. */
    if (!GetLocaleInfoW(MAKELCID(PRIMARYLANGID(id), SORT_DEFAULT),
                        LOCALE_SISO639LANGNAME, out, (int)cch))
        StringCchCopyW(out, cch, L"en");
}

static BOOL TryLoad(const wchar_t *code)
{
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    int     n;

    /* The user's folder first, so a shipped translation can be replaced. */
    if (LangDir(dir, ARRAYSIZE(dir), TRUE) &&
        SUCCEEDED(StringCchPrintfW(path, MAX_PATH, L"%s\\%s.ini", dir, code))) {
        n = LoadFile(path);
        if (n >= 0) {
            Log_Printf(L"lang: %s, %d string(s) from %s", code, n, path);
            return TRUE;
        }
    }
    if (LangDir(dir, ARRAYSIZE(dir), FALSE) &&
        SUCCEEDED(StringCchPrintfW(path, MAX_PATH, L"%s\\%s.ini", dir, code))) {
        n = LoadFile(path);
        if (n >= 0) {
            Log_Printf(L"lang: %s, %d string(s) from %s", code, n, path);
            return TRUE;
        }
    }
    return FALSE;
}

void Lang_Init(const wchar_t *code)
{
    wchar_t want[8];

    Lang_Shutdown();

    if (code && code[0] && _wcsicmp(code, L"auto") != 0)
        StringCchCopyW(want, ARRAYSIZE(want), code);
    else
        CodeFromWindows(want, ARRAYSIZE(want));

    if (TryLoad(want)) {
        StringCchCopyW(g_current, ARRAYSIZE(g_current), want);
        return;
    }

    /* No file for that language. English is compiled in, so this is not a
       failure - it is simply what an untranslated build looks like. */
    if (_wcsicmp(want, L"en") != 0)
        Log_Printf(L"lang: no file for %s, using the built-in English", want);
    StringCchCopyW(g_current, ARRAYSIZE(g_current), L"en");
    TryLoad(L"en");
}

int Lang_List(wchar_t codes[][8], int max)
{
    int pass, count = 0;

    for (pass = 0; pass < 2; pass++) {
        wchar_t          dir[MAX_PATH], glob[MAX_PATH];
        WIN32_FIND_DATAW fd;
        HANDLE           find;

        if (!LangDir(dir, ARRAYSIZE(dir), pass == 0))
            continue;
        if (FAILED(StringCchPrintfW(glob, MAX_PATH, L"%s\\*.ini", dir)))
            continue;

        find = FindFirstFileW(glob, &fd);
        if (find == INVALID_HANDLE_VALUE)
            continue;
        do {
            wchar_t code[8];
            int     i;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            if (FAILED(StringCchCopyW(code, ARRAYSIZE(code), fd.cFileName)))
                continue;              /* longer than a tag; not one of ours */
            PathRemoveExtensionW(code);
            if (!code[0])
                continue;

            for (i = 0; i < count; i++)   /* the two folders may both have it */
                if (_wcsicmp(codes[i], code) == 0)
                    break;
            if (i == count && count < max)
                StringCchCopyW(codes[count++], 8, code);
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    return count;
}

void Lang_Shutdown(void)
{
    int i;

    for (i = 0; i < STR_COUNT; i++) {
        if (g_loaded[i]) {
            HeapFree(GetProcessHeap(), 0, g_loaded[i]);
            g_loaded[i] = NULL;
        }
    }
}
