/* Walking and casing strings the way an installer does.
 *
 * These are the functions a Unicode NSIS installer uses on every path it
 * handles, and the reason one of them being absent stopped an install dead.
 * They are also the functions where being *wrong* is worse than being
 * missing: CharNextW that steps two bytes over a surrogate pair lands in the
 * middle of a character, and the path it builds is quietly corrupt rather
 * than obviously broken. So the surrogate cases are checked explicitly, with
 * a real pair -- U+1F600, four bytes of UTF-16 -- rather than only ASCII.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int checks, failures;
static void ok(int cond, const char *what) {
    checks++;
    if (!cond) { failures++; printf("FAIL %s\n", what); }
    else printf("ok   %s\n", what);
}

int main(void) {
    /* ---- narrow ---- */
    char a[] = "C:\\Program Files\\Game";
    ok(CharNextA(a) == a + 1, "CharNextA steps one byte");
    ok(CharNextA(a + strlen(a)) == a + strlen(a),
       "CharNextA at the terminator stays there");
    ok(CharPrevA(a, a + 3) == a + 2, "CharPrevA steps back one byte");
    ok(CharPrevA(a, a) == a, "CharPrevA at the start stays there");

    /* ---- wide, the plain case ---- */
    WCHAR w[] = L"C:\\Program Files\\Game";
    ok(CharNextW(w) == w + 1, "CharNextW steps one UTF-16 unit");
    size_t wl = wcslen(w);
    ok(CharNextW(w + wl) == w + wl, "CharNextW at the terminator stays there");
    ok(CharPrevW(w, w + 3) == w + 2, "CharPrevW steps back one unit");
    ok(CharPrevW(w, w) == w, "CharPrevW at the start stays there");

    /* ---- wide, the case that corrupts a path if it is wrong ----
     * U+1F600 is D83D DE00: one character, two UTF-16 units. Stepping into
     * the middle of it produces a lone surrogate, which is not a character
     * at all. */
    WCHAR pair[] = { 0xD83D, 0xDE00, 'x', 0 };
    ok(CharNextW(pair) == pair + 2, "CharNextW steps over a whole surrogate pair");
    ok(CharNextW(pair + 2) == pair + 3, "and one unit for the character after it");
    ok(CharPrevW(pair, pair + 2) == pair, "CharPrevW steps back over a whole pair");
    /* A lone high surrogate with no low one following is not a pair, and
     * treating it as one would step past the terminator. */
    WCHAR lone[] = { 0xD83D, 0, 0 };
    ok(CharNextW(lone) == lone + 1, "an unpaired surrogate is one unit");

    /* ---- case, on a string in place ---- */
    char mix[] = "Setup.EXE";
    ok(CharUpperA(mix) == mix && strcmp(mix, "SETUP.EXE") == 0,
       "CharUpperA uppercases in place and returns the pointer");
    ok(CharLowerA(mix) == mix && strcmp(mix, "setup.exe") == 0,
       "CharLowerA lowercases in place");

    WCHAR wmix[] = L"Setup.EXE";
    CharUpperW(wmix);
    ok(wcscmp(wmix, L"SETUP.EXE") == 0, "CharUpperW uppercases the wide string");
    CharLowerW(wmix);
    ok(wcscmp(wmix, L"setup.exe") == 0, "CharLowerW lowercases the wide string");

    /* ---- case, on a single character packed into the low word ---- */
    ok((int)(INT_PTR)CharUpperA((LPSTR)(INT_PTR)'q') == 'Q',
       "CharUpperA on a packed character returns the character");
    ok((int)(INT_PTR)CharLowerA((LPSTR)(INT_PTR)'Q') == 'q',
       "CharLowerA on a packed character returns the character");

    /* ---- the counted forms, where the count is in characters ---- */
    char buf[] = "abcdef";
    ok(CharUpperBuffA(buf, 3) == 3 && strcmp(buf, "ABCdef") == 0,
       "CharUpperBuffA touches exactly the count it was given");
    WCHAR wbuf[] = L"abcdef";
    ok(CharLowerBuffW(wbuf, 6) == 6 && wcscmp(wbuf, L"abcdef") == 0,
       "CharLowerBuffW counts characters, not bytes");
    WCHAR wbuf2[] = L"ABCDEF";
    CharLowerBuffW(wbuf2, 3);
    ok(wcscmp(wbuf2, L"abcDEF") == 0, "and stops at the count");

    /* ---- classification ---- */
    ok(IsCharAlphaA('k') && !IsCharAlphaA('7'), "IsCharAlphaA");
    ok(IsCharAlphaW(L'k') && !IsCharAlphaW(L'7'), "IsCharAlphaW");
    ok(IsCharAlphaNumericA('7') && !IsCharAlphaNumericA('-'), "IsCharAlphaNumericA");
    ok(IsCharUpperA('K') && !IsCharUpperA('k'), "IsCharUpperA");
    ok(IsCharLowerA('k') && !IsCharLowerA('K'), "IsCharLowerA");

    /* ---- what NSIS actually does with them: walk a path ----
     * Not a synthetic loop: this is the shape of the code an installer uses
     * to find the last component of a path, and it is where an off-by-one
     * step shows up as a wrong destination rather than an error. */
    {
        WCHAR path[] = L"C:\\Program Files\\Fake Game\\bin\\game.exe";
        LPWSTR last = path;
        for (LPWSTR p = path; *p; p = CharNextW(p))
            if (*p == L'\\') last = CharNextW(p);
        ok(wcscmp(last, L"game.exe") == 0, "CharNextW finds the last path component");
    }
    {
        char path[] = "C:\\Program Files\\Fake Game\\bin\\game.exe";
        LPSTR last = path;
        for (LPSTR p = path; *p; p = CharNextA(p))
            if (*p == '\\') last = CharNextA(p);
        ok(strcmp(last, "game.exe") == 0, "CharNextA finds the last path component");
    }

    /* ---- wvsprintf: the same formatter, given a va_list ---- */
    {
        WCHAR out[128];
        wsprintfW(out, L"%s\\%s", L"C:\\Games", L"Digital Tamers");
        ok(wcscmp(out, L"C:\\Games\\Digital Tamers") == 0, "wsprintfW builds a path");
    }

    /* ---- lstrcpyW, which copies units and not bytes ---- */
    {
        WCHAR dst[64];
        ok(lstrcpyW(dst, L"C:\\Program Files") == dst
           && wcscmp(dst, L"C:\\Program Files") == 0,
           "lstrcpyW copies the whole wide string");
        ok(lstrlenW(dst) == 16, "and lstrlenW agrees about its length");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
