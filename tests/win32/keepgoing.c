/* What -k should do, and what it used to do instead.
 *
 * `winrun -k` lets a call to a function we have not implemented return
 * something rather than ending the run, so that one pass names *everything*
 * a program needs instead of one name per rebuild. That only works if the
 * something it returns can survive being used.
 *
 * It used to be zero. For anything returning a pointer, zero is a null the
 * caller walks straight into: a Unicode installer that called CharNextW went
 * into `movzx ecx, word ptr [eax]` with eax = 0 and faulted on the first
 * lie, so the run that was supposed to produce a list produced one name and
 * a crash address. Now it is a pointer to a page of zeros -- non-null, so a
 * dereference reads; zero-filled, so it reads as an empty string of either
 * width and as a zeroed structure.
 *
 * The functions used here are deliberately ones this layer will never have:
 * there are no desktops inside a sandbox holding one program, and nothing
 * here can lock a workstation. If one of them ever *is* implemented this
 * test will fail loudly -- and the fix is to pick another function that is
 * genuinely out of scope, not to delete the test.
 */
#include <windows.h>
#include <stdio.h>

static int checks, failures;
static void ok(int cond, const char *what) {
    checks++;
    if (!cond) { failures++; printf("FAIL %s\n", what); }
    else printf("ok   %s\n", what);
}

int main(void) {
    /* Not implemented, and returns a BOOL. Under -k the run carries on. */
    BOOL b = SwitchDesktop(NULL);
    ok(1, "a call to a function that does not exist returned at all");
    (void)b;

    /* Not implemented either -- so the run got past the first one, which is
     * the whole point of -k. */
    LockWorkStation();
    ok(1, "and so did a second one");

    /* And the dangerous case: use the result as a pointer. This is the exact
     * shape that used to end the run -- read a UTF-16 unit through whatever
     * came back. It has to read, and it has to read zero. */
    HDESK h = CreateDesktopW(L"x", NULL, NULL, 0, 0, NULL);
    ok(h != NULL, "a function that returns a handle did not return NULL");
    const WCHAR *p = (const WCHAR *)h;
    ok(p[0] == 0, "and what it points at reads as an empty wide string");
    ok(((const char *)h)[0] == 0, "and as an empty narrow one");

    /* Writable, so a caller that fills in the buffer it was handed does not
     * fault either. */
    ((char *)h)[0] = 'a';
    ok(((const char *)h)[0] == 'a', "and can be written to");

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
