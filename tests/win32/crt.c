/* A normal mingw-w64 program: the C runtime (msvcrt.dll) does the startup,
 * stdio, heap and math. Getting this to print is the milestone that says the
 * Win32 layer is real: it is what every game's CRT does before main(). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct node { int v; struct node *next; } node;

int main(int argc, char **argv) {
    printf("argc=%d argv0=%s\n", argc, strrchr(argv[0], '\\') ? strrchr(argv[0], '\\') + 1 : argv[0]);
    node *head = NULL;
    for (int i = 0; i < 100; i++) { node *n = malloc(sizeof *n); n->v = i * i; n->next = head; head = n; }
    long sum = 0; for (node *n = head; n; n = n->next) sum += n->v;
    printf("sum=%ld\n", sum);
    while (head) { node *n = head->next; free(head); head = n; }
    double x = 0; for (int i = 1; i <= 1000; i++) x += sqrt((double)i) / (double)i;
    printf("x=%.6f\n", x);
    char *s = calloc(1, 64); snprintf(s, 64, "%s-%d-%x", "fmt", 42, 0xBEEF); puts(s); free(s);
    return 3;
}
