/* Finding files and mapping them -- how a game opens its data.
 *
 * A wildcard walk over a directory, then the same file reached two ways: read
 * through a handle, and mapped into the address space. A game does the second
 * because an archive is too big to read; this checks the bytes agree either
 * way, which is the only thing that matters about a mapping.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

int main(void) {
    /* the wildcard walk. Order from a directory is not defined, so sort it. */
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("C:\\data\\*.bsa", &fd);
    printf("FindFirstFile(*.bsa): %s\n", h == INVALID_HANDLE_VALUE ? "nothing found" : "found something");
    if (h == INVALID_HANDLE_VALUE) return 1;
    char names[16][64]; int n = 0;
    do {
        if (n < 16) { snprintf(names[n], 64, "%s (%lu bytes, attr %#lx)", fd.cFileName,
                               (unsigned long)fd.nFileSizeLow, (unsigned long)fd.dwFileAttributes); n++; }
    } while (FindNextFileA(h, &fd));
    printf("FindNextFile ended with error %lu (18 = no more files)\n", (unsigned long)GetLastError());
    FindClose(h);
    char *ptrs[16]; for (int i = 0; i < n; i++) ptrs[i] = names[i];
    qsort(ptrs, n, sizeof ptrs[0], cmp);
    printf("%d matched:\n", n);
    for (int i = 0; i < n; i++) printf("  %s\n", ptrs[i]);

    /* a pattern that matches nothing must say so, not invent an entry */
    HANDLE none = FindFirstFileA("C:\\data\\*.esm", &fd);
    printf("a pattern matching nothing: %s (error %lu)\n",
           none == INVALID_HANDLE_VALUE ? "INVALID_HANDLE_VALUE" : "FAILED - returned a handle",
           (unsigned long)GetLastError());

    /* the same file, read and mapped */
    HANDLE f = CreateFileA("C:\\data\\one.bsa", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { printf("could not open one.bsa\n"); return 1; }
    char viaRead[64] = {0}; DWORD got = 0;
    ReadFile(f, viaRead, 40, &got, NULL);

    HANDLE map = CreateFileMappingA(f, NULL, PAGE_READONLY, 0, 0, NULL);
    printf("CreateFileMapping: %s\n", map ? "ok" : "FAILED");
    const char *view = (const char *)MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
    printf("MapViewOfFile: %s\n", view ? "ok" : "FAILED");
    if (view) {
        printf("mapped bytes match the read bytes: %s\n",
               (DWORD)strlen(view) >= got && memcmp(view, viaRead, got) == 0 ? "yes" : "NO");
        printf("first line through the mapping: %.11s\n", view);
        UnmapViewOfFile(view);
    }
    CloseHandle(map);
    CloseHandle(f);
    return 0;
}
