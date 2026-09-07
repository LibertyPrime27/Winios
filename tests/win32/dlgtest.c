/* A dialog, drawn.
 *
 * Everything before this could create a window and pump its messages; none
 * of it put a pixel on the screen, which is why an installer "ran" and
 * showed nothing. This guest exercises the whole path: a real RT_DIALOG
 * resource in its own image, walked by the loader; controls created from
 * the template; text set and read back; a progress bar moved; a button
 * clicked, and the WM_COMMAND that click produces ending the dialog.
 *
 * What it prints is what the guest can check for itself. What it *draws* is
 * checked outside, by winrun's -screen dump, because a program cannot see
 * its own pixels and a claim that it drew correctly is worth nothing without
 * something looking.
 */
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>

#define IDC_HEAD  200
#define IDC_LABEL 201
#define IDC_EDIT  202
#define IDC_CHECK 203
#define IDC_PROG  204
#define IDC_STAT  205

static int checks, failures;
static void ok(int cond, const char *what) {
    checks++;
    if (!cond) { failures++; printf("FAIL %s\n", what); }
    else printf("ok   %s\n", what);
}

static INT_PTR CALLBACK dlgproc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        ok(lp == 0x1234, "WM_INITDIALOG carries the DialogBoxParam parameter");

        HWND edit = GetDlgItem(dlg, IDC_EDIT);
        HWND prog = GetDlgItem(dlg, IDC_PROG);
        HWND btn  = GetDlgItem(dlg, IDOK);
        ok(edit != NULL, "GetDlgItem finds the edit control");
        ok(prog != NULL, "GetDlgItem finds the progress bar");
        ok(btn  != NULL, "GetDlgItem finds the default button");
        ok(GetDlgItem(dlg, 999) == NULL, "GetDlgItem returns NULL for an id that is not there");
        ok(GetDlgCtrlID(edit) == IDC_EDIT, "GetDlgCtrlID gives the id back");

        /* The caption came out of the template, not out of the program. */
        char cap[64] = "";
        GetWindowTextA(dlg, cap, sizeof cap);
        ok(strcmp(cap, "Winios Setup") == 0, "the dialog's caption came from the template");

        char btext[64] = "";
        GetWindowTextA(btn, btext, sizeof btext);
        ok(strcmp(btext, "&Install") == 0, "a control's text came from the template");

        SetDlgItemTextA(dlg, IDC_EDIT, "C:\\Program Files\\Winios");
        char back[128] = "";
        GetDlgItemTextA(dlg, IDC_EDIT, back, sizeof back);
        ok(strcmp(back, "C:\\Program Files\\Winios") == 0, "SetDlgItemText round-trips");

        SendDlgItemMessageA(dlg, IDC_PROG, PBM_SETRANGE32, 0, 200);
        SendDlgItemMessageA(dlg, IDC_PROG, PBM_SETPOS, 120, 0);
        ok(SendDlgItemMessageA(dlg, IDC_PROG, PBM_GETPOS, 0, 0) == 120, "the progress bar keeps its position");

        CheckDlgButton(dlg, IDC_CHECK, BST_CHECKED);
        ok(IsDlgButtonChecked(dlg, IDC_CHECK) == BST_CHECKED, "a check box checks");

        SetDlgItemTextA(dlg, IDC_STAT, "Copying files...");

        /* Dialog units are not pixels, and a control placed with the wrong
         * conversion is in the wrong place even though nothing errored. */
        RECT r = { 0, 0, 4, 8 };
        MapDialogRect(dlg, &r);
        ok(r.right > 0 && r.bottom > 0, "MapDialogRect converts dialog units to pixels");

        RECT cr;
        GetClientRect(dlg, &cr);
        ok(cr.right > 100 && cr.bottom > 60, "the dialog is the size its template asked for");

        /* Paint everything now, so the screen dump below is of a finished
         * dialog rather than of whatever had been drawn by this point. */
        UpdateWindow(dlg);

        /* And now click Install, the way a person would. The click has to
         * come back as a WM_COMMAND from the button, not be faked here. */
        SendMessageA(btn, BM_CLICK, 0, 0);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            ok(HIWORD(wp) == BN_CLICKED, "the button reported BN_CLICKED");
            ok((HWND)lp == GetDlgItem(dlg, IDOK), "the notification named the button");
            EndDialog(dlg, 42);
            return TRUE;
        }
        return FALSE;
    }
    return FALSE;
}

/* Drawing straight onto a window, which is what a program that does not use
 * a dialog does. */
static void draw_direct(void) {
    HWND w = CreateWindowExA(0, "STATIC", "direct", WS_POPUP | WS_VISIBLE,
                             40, 40, 300, 120, NULL, NULL, GetModuleHandleA(NULL), NULL);
    ok(w != NULL, "a plain window can be created");
    HDC dc = GetDC(w);
    ok(dc != NULL, "GetDC gives a device context");

    HBRUSH br = CreateSolidBrush(RGB(0x20, 0x40, 0x80));
    RECT r = { 0, 0, 300, 120 };
    ok(FillRect(dc, &r, br) != 0, "FillRect fills");

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    ok(TextOutA(dc, 8, 8, "Winios", 6) != 0, "TextOut draws");

    SIZE sz;
    ok(GetTextExtentPoint32A(dc, "Winios", 6, &sz) != 0 && sz.cx > 0 && sz.cy > 0,
       "text has a measurable size");

    RECT tr = { 8, 40, 292, 112 };
    int h = DrawTextA(dc, "The quick brown fox jumps over the lazy dog, and keeps going "
                          "until it has to wrap onto another line.", -1, &tr,
                      DT_LEFT | DT_WORDBREAK);
    ok(h > sz.cy, "DrawText with DT_WORDBREAK used more than one line");

    TEXTMETRICA tm;
    ok(GetTextMetricsA(dc, &tm) != 0 && tm.tmHeight > 0, "the font has metrics");
    ok(GetDeviceCaps(dc, BITSPIXEL) == 32, "the display is 32 bits per pixel");

    HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 220, 0));
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, 8, 30, NULL);
    ok(LineTo(dc, 292, 30) != 0, "LineTo draws");
    SelectObject(dc, old);

    DeleteObject(pen);
    DeleteObject(br);
    ReleaseDC(w, dc);
    DestroyWindow(w);
}

int main(void) {
    /* What an installer calls first, by ordinal. */
    InitCommonControls();

    draw_direct();

    INT_PTR r = DialogBoxParamA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(100),
                                NULL, dlgproc, 0x1234);
    ok(r == 42, "DialogBoxParam returned what EndDialog was given");

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
