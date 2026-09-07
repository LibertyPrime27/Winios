/* Everything a modern game engine links against, imported and called.
 *
 * A GameMaker game resolved 232 of its imports and was missing 114, spread
 * across nineteen DLLs -- most of which did not exist here at all. An import
 * that cannot be resolved stops the program before it runs a line of its own
 * code, so those 114 were not 114 features missing; they were one program
 * not starting.
 *
 * This guest imports the same set and calls each one. What it asserts is
 * mostly not "the right thing happened" -- there is no network to connect
 * to, no IME, no file picker -- but "this returned, with an answer its
 * caller can act on". A game asking whether it is connected to the internet
 * needs a yes or a no; what it cannot survive is the question not existing.
 *
 * The exception is Direct3D 11, which is checked to *fail*, and to fail with
 * the code that means "no device here can do this". If that ever starts
 * succeeding, this test should be the thing that notices.
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <commdlg.h>
#include <dbghelp.h>
#include <dwmapi.h>
#include <imm.h>
#include <rpc.h>
#include <shlwapi.h>
#include <wininet.h>
#include <d3d11.h>
#include <stdio.h>
#include <string.h>

static int checks, failures;
static void ok(int cond, const char *what) {
    checks++;
    if (!cond) { failures++; printf("FAIL %s\n", what); }
    else printf("ok   %s\n", what);
}

int main(void) {
    /* ---- the locale block a C runtime walks before main ---------------- */
    {
        WCHAR buf[64];
        int n = GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_SDECIMAL, buf, 64);
        ok(n > 0, "GetLocaleInfoW answers the CRT's locale query");
        ok(IsValidLocale(LOCALE_USER_DEFAULT, LCID_INSTALLED), "IsValidLocale");

        WCHAR up[16] = L"abc";
        int m = LCMapStringW(LOCALE_USER_DEFAULT, LCMAP_UPPERCASE, L"abc", 3, up, 16);
        ok(m > 0 && up[0] == L'A', "LCMapStringW uppercases");

        int r = CompareStringW(LOCALE_USER_DEFAULT, 0, L"a", 1, L"b", 1);
        ok(r == CSTR_LESS_THAN, "CompareStringW orders two strings");
    }

    /* ---- the version gate --------------------------------------------- */
    {
        OSVERSIONINFOEXW vi;
        memset(&vi, 0, sizeof vi);
        vi.dwOSVersionInfoSize = sizeof vi;
        vi.dwMajorVersion = 6;
        vi.dwMinorVersion = 1;                      /* "Windows 7 or later?" */
        DWORDLONG mask = 0;
        VER_SET_CONDITION(mask, VER_MAJORVERSION, VER_GREATER_EQUAL);
        VER_SET_CONDITION(mask, VER_MINORVERSION, VER_GREATER_EQUAL);
        ok(VerifyVersionInfoW(&vi, VER_MAJORVERSION | VER_MINORVERSION, mask) != 0,
           "VerifyVersionInfo passes a Windows 7 check");
    }

    /* ---- times, which a save file is stamped with ---------------------- */
    {
        SYSTEMTIME st;
        FILETIME ft;
        GetSystemTime(&st);
        ok(SystemTimeToFileTime(&st, &ft) != 0, "SystemTimeToFileTime");
        SYSTEMTIME back;
        ok(FileTimeToSystemTime(&ft, &back) != 0 && back.wYear == st.wYear,
           "FileTimeToSystemTime round-trips the year");
        SYSTEMTIME local;
        ok(SystemTimeToTzSpecificLocalTime(NULL, &st, &local) != 0,
           "SystemTimeToTzSpecificLocalTime");
        WCHAR d[64];
        ok(GetDateFormatW(LOCALE_USER_DEFAULT, 0, &st, NULL, d, 64) > 0, "GetDateFormatW");
        ok(GetTimeFormatW(LOCALE_USER_DEFAULT, 0, &st, NULL, d, 64) > 0, "GetTimeFormatW");
    }

    /* ---- entropy ------------------------------------------------------- */
    {
        HCRYPTPROV prov = 0;
        ok(CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) != 0,
           "CryptAcquireContext");
        BYTE a[32], b[32];
        memset(a, 0, sizeof a); memset(b, 0, sizeof b);
        ok(CryptGenRandom(prov, sizeof a, a) != 0, "CryptGenRandom");
        CryptGenRandom(prov, sizeof b, b);
        /* Two draws being identical is possible and astronomically unlikely;
         * a fixed seed would make it certain, which is the bug worth
         * catching. */
        ok(memcmp(a, b, sizeof a) != 0, "and two draws differ");
        ok(CryptReleaseContext(prov, 0) != 0, "CryptReleaseContext");
    }

    /* ---- files, in the shapes an engine asks about them ----------------- */
    {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        ok(GetFileAttributesExW(L"C:\\", GetFileExInfoStandard, &fad) != 0,
           "GetFileAttributesExW on the drive root");
        ok(GetDriveTypeW(L"C:\\") == DRIVE_FIXED, "GetDriveTypeW says the drive is fixed");
    }

    /* ---- error text ---------------------------------------------------- */
    {
        WCHAR msg[256];
        DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM, NULL, ERROR_FILE_NOT_FOUND,
                                 0, msg, 256, NULL);
        ok(n > 0, "FormatMessageW turns an error code into a sentence");
    }

    /* ---- the IME that is not here -------------------------------------- */
    {
        HWND w = CreateWindowExA(0, "STATIC", "", WS_POPUP, 0, 0, 64, 64,
                                 NULL, NULL, GetModuleHandleA(NULL), NULL);
        HIMC imc = ImmGetContext(w);
        ok(imc == NULL, "ImmGetContext reports no IME");
        ImmReleaseContext(w, imc);
        ok(ImmAssociateContext(w, NULL) == NULL, "ImmAssociateContext");

        /* rectangles, which a game does its own layout with */
        RECT a = { 0, 0, 10, 10 }, b = { 5, 5, 20, 20 }, out;
        ok(IntersectRect(&out, &a, &b) != 0 && out.left == 5 && out.right == 10,
           "IntersectRect");
        ok(PtInRect(&a, (POINT){ 3, 3 }) != 0, "PtInRect");
        POINT p = { 1, 1 };
        MapWindowPoints(w, NULL, &p, 1);
        ok(1, "MapWindowPoints returned");
        WINDOWPLACEMENT wp;
        wp.length = sizeof wp;
        ok(GetWindowPlacement(w, &wp) != 0, "GetWindowPlacement");
        ok(SetLayeredWindowAttributes(w, 0, 128, LWA_ALPHA) != 0, "SetLayeredWindowAttributes");
        BYTE alpha = 0;
        DWORD flags = 0; COLORREF key = 0;
        ok(GetLayeredWindowAttributes(w, &key, &alpha, &flags) != 0 && alpha == 128,
           "and it reads back what was set");
        DestroyWindow(w);
    }

    /* ---- composition timing, which there is none of --------------------- */
    {
        BOOL on = TRUE;
        ok(DwmIsCompositionEnabled(&on) == S_OK && !on,
           "DwmIsCompositionEnabled says there is no compositor");
    }

    /* ---- a UUID -------------------------------------------------------- */
    {
        UUID a, b;
        ok(UuidCreate(&a) == RPC_S_OK, "UuidCreate");
        UuidCreate(&b);
        ok(memcmp(&a, &b, sizeof a) != 0, "and two are different");
        RPC_WSTR s = NULL;
        ok(UuidToStringW(&a, &s) == RPC_S_OK && s != NULL && wcslen((WCHAR *)s) == 36,
           "UuidToStringW gives a 36-character form");
        RpcStringFreeW(&s);
    }

    /* ---- networking, which is deliberately absent ----------------------- */
    {
        WSADATA wsa;
        ok(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "WSAStartup succeeds");
        ok(htons(0x1234) == 0x3412, "htons swaps");
        ok(inet_addr("1.2.3.4") == 0x04030201u, "inet_addr parses");
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        ok(s == INVALID_SOCKET, "socket fails, because there is no network here");
        ok(WSAGetLastError() == WSAENETDOWN, "and says the network is down");
        WSACleanup();

        HINTERNET h = InternetOpenA("test", 0, NULL, NULL, 0);
        ok(h != NULL, "InternetOpen succeeds so a program gets past its setup");
        DWORD f = 0;
        ok(InternetGetConnectedState(&f, 0) == FALSE, "and reports no connection");
        InternetCloseHandle(h);
    }

    /* ---- adapters, of which there are none ------------------------------ */
    {
        ULONG size = 0;
        ok(GetAdaptersAddresses(AF_UNSPEC, 0, NULL, NULL, &size) == ERROR_NO_DATA,
           "GetAdaptersAddresses reports no adapters");
    }

    /* ---- the file picker that cannot be shown --------------------------- */
    {
        OPENFILENAMEW ofn;
        WCHAR file[MAX_PATH] = L"";
        memset(&ofn, 0, sizeof ofn);
        ofn.lStructSize = sizeof ofn;
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ok(GetOpenFileNameW(&ofn) == FALSE, "GetOpenFileName is cancelled, not broken");
        ok(CommDlgExtendedError() == 0, "and reports cancellation rather than an error");
    }

    /* ---- the joystick port that is empty -------------------------------- */
    {
        JOYINFO ji;
        ok(joyGetPos(0, &ji) == JOYERR_UNPLUGGED, "joyGetPos: nothing plugged in");
    }

    /* ---- and Direct3D 11, which is the one that is genuinely missing ---- */
    {
        ID3D11Device *dev = NULL;
        ID3D11DeviceContext *ctx = NULL;
        D3D_FEATURE_LEVEL got = (D3D_FEATURE_LEVEL)0;
        HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                       NULL, 0, D3D11_SDK_VERSION, &dev, &got, &ctx);
        ok(FAILED(hr), "D3D11CreateDevice fails -- Direct3D 11 is not implemented");
        ok(hr == DXGI_ERROR_UNSUPPORTED,
           "and fails with 'no device here can do that', which a game handles");
        ok(dev == NULL && ctx == NULL, "and hands back nothing to use");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
