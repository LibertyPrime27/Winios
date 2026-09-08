/* Networking, which is here so that games start without it.
 *
 * A game engine links Winsock and WinINet whether or not the game uses them:
 * the runtime supports multiplayer and update checks, so the imports are in
 * every build. The GameMaker runner imports thirty-seven of these and a
 * single-player game calls perhaps two.
 *
 * Nothing here opens a socket, and that is a decision rather than an
 * omission. A Windows program running inside this app would be reaching the
 * network as the app, on someone's phone, with no way for them to see it
 * happening or stop it -- and "the game I installed quietly talked to a
 * server" is not something to arrange by accident on the way to fixing an
 * import table. If it should be allowed later it should be a setting with a
 * switch, not a side effect.
 *
 * So: the calls that set a program up succeed, and the calls that would
 * carry traffic fail the way they fail on a machine with the cable out.
 * Every one of those failures is a documented code the caller already
 * handles, because "no network" is a state every networked program is
 * written to cope with. What none of them do is hang, and none of them
 * claim to have sent anything.
 */
#define _GNU_SOURCE
#include "w32.h"

#include <stdio.h>
#include <string.h>

/* ----------------------------------------------------------------- ws2_32
 *
 * Winsock exports by ordinal as well as by name, and a program built against
 * the import library gets the ordinals -- which is why an import table shows
 * "#115" rather than "WSAStartup". The numbers have been fixed since Winsock
 * 1.1 in 1993 and are part of the ABI; the map below is that contract, and
 * it is what turns a column of numbers in the run report into names somebody
 * can act on.
 */
static const struct { int ord; const char *name; } WS2_ORD[] = {
    {   1, "accept"          }, {   2, "bind"            }, {   3, "closesocket"  },
    {   4, "connect"         }, {   5, "getpeername"     }, {   6, "getsockname"  },
    {   7, "getsockopt"      }, {   8, "htonl"           }, {   9, "htons"        },
    {  10, "ioctlsocket"     }, {  11, "inet_addr"       }, {  12, "inet_ntoa"    },
    {  13, "listen"          }, {  14, "ntohl"           }, {  15, "ntohs"        },
    {  16, "recv"            }, {  17, "recvfrom"        }, {  18, "select"       },
    {  19, "send"            }, {  20, "sendto"          }, {  21, "setsockopt"   },
    {  22, "shutdown"        }, {  23, "socket"          },
    {  51, "gethostbyaddr"   }, {  52, "gethostbyname"   }, {  53, "getprotobyname" },
    {  54, "getprotobynumber" }, { 55, "getservbyname"   }, {  56, "getservbyport" },
    {  57, "gethostname"     },
    { 111, "WSAGetLastError" }, { 112, "WSASetLastError" },
    { 113, "WSACancelBlockingCall" }, { 114, "WSAIsBlocking" },
    { 115, "WSAStartup"      }, { 116, "WSACleanup"      },
    { 151, "__WSAFDIsSet"    },
};

const char *w32_ws2_ordinal_name(int ordinal) {
    for (size_t i = 0; i < sizeof WS2_ORD / sizeof WS2_ORD[0]; i++)
        if (WS2_ORD[i].ord == ordinal) return WS2_ORD[i].name;
    return 0;
}

enum {
    WSAENETDOWN_    = 10050,
    WSAENOTSOCK_    = 10038,
    WSAEHOSTUNREACH_ = 10065,
    WSANOTINITIALISED_ = 10093, WSAEFAULT_ = 10014,
    WSAHOST_NOT_FOUND_ = 11001,
};
/* A SOCKET is a UINT_PTR: 32 bits on x86 and 64 on x64, and INVALID_SOCKET
 * is all-ones at whichever width. Returning a 32-bit ~0 on a 64-bit build
 * gives 0x00000000FFFFFFFF, which is not equal to INVALID_SOCKET and is
 * therefore a *valid* socket as far as the caller is concerned -- so a
 * program that correctly checked for failure carries on and uses it.
 * SOCKET_ERROR is an int and stays 32-bit. */
#define INVALID_SOCKET_ ((uint64_t)~0ull)
#define SOCKET_ERROR_   ((uint64_t)(uint32_t)-1)

static int g_ws_started;
static uint32_t g_ws_error;

static void ws_fail(w32 *w, int err) { (void)w; g_ws_error = (uint32_t)err; }

/* WSAStartup succeeds. It has to: a game that cannot initialise Winsock
 * often treats that as a broken system rather than as a system with no
 * network, and stops. What it gets afterwards is an honest "the network is
 * down" from every call that would use one. */
static void ws_WSAStartup(w32 *w) {
    uint64_t d = ARG(1);
    if (d) {
        /* WSADATA: version, high version, then a description and status
         * string, and on 32-bit the socket count before them. Filling the
         * version and clearing the rest is enough for every caller. */
        void *p = W32PN(w, d, w->is32 ? 400 : 408);
        if (!p) { ws_fail(w, WSAEFAULT_); RET(WSAEFAULT_); return; }
        memset(p, 0, w->is32 ? 400 : 408);
        w32_write(w, d, 2, ARG(0) ? ARG(0) : 0x0202);
        w32_write(w, d + 2, 2, 0x0202);
    }
    g_ws_started = 1;
    g_ws_error = 0;
    RET(0);
}
static void ws_WSACleanup(w32 *w) { (void)w; g_ws_started = 0; RET(0); }
static void ws_WSAGetLastError(w32 *w) { RET(g_ws_started ? g_ws_error : (uint32_t)WSANOTINITIALISED_); }
static void ws_WSASetLastError(w32 *w) { g_ws_error = (uint32_t)ARG(0); RET(0); }
static void ws_WSAIsBlocking(w32 *w) { (void)w; RET(0); }
static void ws_WSACancelBlockingCall(w32 *w) { (void)w; RET(0); }
static void ws___WSAFDIsSet(w32 *w) { (void)w; RET(0); }

/* Byte order, which is arithmetic and has nothing to do with a network. */
static uint32_t bswap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}
static void ws_htonl(w32 *w) { RET(bswap32((uint32_t)ARG(0))); }
static void ws_ntohl(w32 *w) { RET(bswap32((uint32_t)ARG(0))); }
static void ws_htons(w32 *w) { uint32_t v = (uint32_t)ARG(0) & 0xFFFF; RET(((v >> 8) | (v << 8)) & 0xFFFF); }
static void ws_ntohs(w32 *w) { ws_htons(w); }

/* Address text to bytes and back, which is also just parsing. */
static void ws_inet_addr(w32 *w) {
    const char *s = ARG(0) ? w32_str(w, ARG(0)) : "";
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255) {
        RET(0xFFFFFFFFu);                            /* INADDR_NONE */
        return;
    }
    RET((uint32_t)(a | (b << 8) | (c << 16) | (d << 24)));
}
static void ws_inet_ntoa(w32 *w) {
    uint32_t v = (uint32_t)ARG(0);
    char buf[24];
    snprintf(buf, sizeof buf, "%u.%u.%u.%u", v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF);
    /* Winsock returns a pointer to its own static buffer, and so does this --
     * one guest allocation reused, which is the same contract. */
    static uint64_t scratch;
    if (!scratch) scratch = w32_heap_alloc(w, 32);
    for (size_t i = 0; i <= strlen(buf); i++) w32_write(w, scratch + i, 1, (uint8_t)buf[i]);
    RET(scratch);
}
static void ws_inet_pton(w32 *w) {
    const char *s = ARG(1) ? w32_str(w, ARG(1)) : "";
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (ARG(0) != 2 || sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) { RET(0); return; }
    if (ARG(2)) w32_write(w, ARG(2), 4, (uint32_t)(a | (b << 8) | (c << 16) | (d << 24)));
    RET(1);
}
static void ws_inet_ntop(w32 *w) {
    uint64_t src = ARG(1), dst = ARG(2);
    if (ARG(0) != 2 || !src || !dst) { RET(0); return; }
    uint32_t v = (uint32_t)w32_read(w, src, 4);
    char buf[24];
    snprintf(buf, sizeof buf, "%u.%u.%u.%u", v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF);
    for (size_t i = 0; i <= strlen(buf); i++) w32_write(w, dst + i, 1, (uint8_t)buf[i]);
    RET(dst);
}
static void ws_gethostname(w32 *w) {
    const char *n = "winios";
    if (ARG(0) && (int)ARG(1) > (int)strlen(n))
        for (size_t i = 0; i <= strlen(n); i++) w32_write(w, ARG(0) + i, 1, (uint8_t)n[i]);
    RET(0);
}

/* Everything that would carry traffic. The cable is out. */
static void ws_socket(w32 *w)      { ws_fail(w, WSAENETDOWN_); RET(INVALID_SOCKET_); }
static void ws_closesocket(w32 *w) { ws_fail(w, WSAENOTSOCK_); RET(SOCKET_ERROR_); }
static void ws_connect(w32 *w)     { ws_fail(w, WSAENETDOWN_); RET(SOCKET_ERROR_); }
static void ws_bind(w32 *w)        { ws_fail(w, WSAENETDOWN_); RET(SOCKET_ERROR_); }
static void ws_listen(w32 *w)      { ws_fail(w, WSAENETDOWN_); RET(SOCKET_ERROR_); }
static void ws_accept(w32 *w)      { ws_fail(w, WSAENETDOWN_); RET(INVALID_SOCKET_); }
static void ws_send(w32 *w)        { ws_fail(w, WSAENETDOWN_); RET(SOCKET_ERROR_); }
static void ws_sendto(w32 *w)      { ws_fail(w, WSAENETDOWN_); RET(SOCKET_ERROR_); }
static void ws_recv(w32 *w)        { ws_fail(w, WSAENETDOWN_); RET(SOCKET_ERROR_); }
static void ws_recvfrom(w32 *w)    { ws_fail(w, WSAENETDOWN_); RET(SOCKET_ERROR_); }
static void ws_select(w32 *w)      { (void)w; RET(0); }        /* nothing is ready */
static void ws_getsockopt(w32 *w)  { ws_fail(w, WSAENOTSOCK_); RET(SOCKET_ERROR_); }
static void ws_setsockopt(w32 *w)  { ws_fail(w, WSAENOTSOCK_); RET(SOCKET_ERROR_); }
static void ws_ioctlsocket(w32 *w) { ws_fail(w, WSAENOTSOCK_); RET(SOCKET_ERROR_); }
static void ws_shutdown(w32 *w)    { ws_fail(w, WSAENOTSOCK_); RET(SOCKET_ERROR_); }
static void ws_getsockname(w32 *w) { ws_fail(w, WSAENOTSOCK_); RET(SOCKET_ERROR_); }
static void ws_getpeername(w32 *w) { ws_fail(w, WSAENOTSOCK_); RET(SOCKET_ERROR_); }
static void ws_gethostbyname(w32 *w) { ws_fail(w, WSAHOST_NOT_FOUND_); RET(0); }
static void ws_gethostbyaddr(w32 *w) { ws_fail(w, WSAHOST_NOT_FOUND_); RET(0); }
static void ws_getprotobyname(w32 *w) { ws_fail(w, WSAHOST_NOT_FOUND_); RET(0); }
static void ws_getprotobynumber(w32 *w) { ws_fail(w, WSAHOST_NOT_FOUND_); RET(0); }
static void ws_getservbyname(w32 *w) { ws_fail(w, WSAHOST_NOT_FOUND_); RET(0); }
static void ws_getservbyport(w32 *w) { ws_fail(w, WSAHOST_NOT_FOUND_); RET(0); }
static void ws_getaddrinfo(w32 *w) {
    if (ARG(3)) w32_write(w, ARG(3), (int)w32_ptrsize(w), 0);
    RET(WSAHOST_NOT_FOUND_);
}
static void ws_freeaddrinfo(w32 *w) { (void)w; RET(0); }
static void ws_getnameinfo(w32 *w) { RET(WSAHOST_NOT_FOUND_); }
static void ws_WSAAddressToStringA(w32 *w) { ws_fail(w, WSAENETDOWN_); RET(SOCKET_ERROR_); }
static void ws_WSAAddressToStringW(w32 *w) { ws_fail(w, WSAENETDOWN_); RET(SOCKET_ERROR_); }
static void ws_WSAAsyncSelect(w32 *w) { ws_fail(w, WSAENOTSOCK_); RET(SOCKET_ERROR_); }
static void ws_WSAIoctl(w32 *w) { ws_fail(w, WSAENOTSOCK_); RET(SOCKET_ERROR_); }
static void ws_WSAEventSelect(w32 *w) { ws_fail(w, WSAENOTSOCK_); RET(SOCKET_ERROR_); }

/* Registered under both its name and its ordinal, because a program built
 * against the import library asks for one and a program built against a
 * header asks for the other, and they are the same function. */
#define W(n, a)  { #n, a, 0, ws_##n, 0 }
#define WO(o, n, a) { "#" #o, a, 0, ws_##n, 0 }
const w32_api w32_ws2_32[] = {
    W(WSAStartup, 2), W(WSACleanup, 0), W(WSAGetLastError, 0), W(WSASetLastError, 1),
    W(WSAIsBlocking, 0), W(WSACancelBlockingCall, 0), W(__WSAFDIsSet, 2),
    W(htonl, 1), W(ntohl, 1), W(htons, 1), W(ntohs, 1),
    W(inet_addr, 1), W(inet_ntoa, 1), W(inet_pton, 3), W(inet_ntop, 4),
    W(gethostname, 2),
    W(socket, 3), W(closesocket, 1), W(connect, 3), W(bind, 3), W(listen, 2),
    W(accept, 3), W(send, 4), W(sendto, 6), W(recv, 4), W(recvfrom, 6),
    W(select, 5), W(getsockopt, 5), W(setsockopt, 5), W(ioctlsocket, 3),
    W(shutdown, 2), W(getsockname, 3), W(getpeername, 3),
    W(gethostbyname, 1), W(gethostbyaddr, 3),
    W(getprotobyname, 1), W(getprotobynumber, 1),
    W(getservbyname, 2), W(getservbyport, 2),
    W(getaddrinfo, 4), W(freeaddrinfo, 1), W(getnameinfo, 7),
    W(WSAAddressToStringA, 5), W(WSAAddressToStringW, 5),
    W(WSAAsyncSelect, 4), W(WSAIoctl, 9), W(WSAEventSelect, 3),
    { 0, 0, 0, 0, 0 },
};
#undef W
#undef WO

/* ---------------------------------------------------------------- wininet
 *
 * An update check, a leaderboard, a crash report. Opening a session works,
 * so the program gets past its setup; connecting does not, with the code
 * that means "there is no connection". A game does that check on a
 * background thread and carries on. */
enum { ERROR_INTERNET_CANNOT_CONNECT_ = 12029, ERROR_INTERNET_TIMEOUT_ = 12002 };

static void wi_InternetOpenA(w32 *w) { (void)w; RET(0x1E700001u); }
static void wi_InternetOpenW(w32 *w) { (void)w; RET(0x1E700001u); }
static void wi_InternetConnectA(w32 *w) {
    w32_note_refused(w, "wininet!InternetConnect (no network from here)");
    w32_set_last_error(w, ERROR_INTERNET_CANNOT_CONNECT_);
    RET(0);
}
static void wi_InternetConnectW(w32 *w) { wi_InternetConnectA(w); }
static void wi_InternetCloseHandle(w32 *w) { (void)w; RET(1); }
static void wi_HttpOpenRequestA(w32 *w) { w32_set_last_error(w, ERROR_INTERNET_CANNOT_CONNECT_); RET(0); }
static void wi_HttpOpenRequestW(w32 *w) { wi_HttpOpenRequestA(w); }
static void wi_HttpSendRequestA(w32 *w) { w32_set_last_error(w, ERROR_INTERNET_CANNOT_CONNECT_); RET(0); }
static void wi_HttpSendRequestW(w32 *w) { wi_HttpSendRequestA(w); }
static void wi_HttpQueryInfoA(w32 *w) { w32_set_last_error(w, ERROR_INTERNET_CANNOT_CONNECT_); RET(0); }
static void wi_InternetReadFile(w32 *w) {
    /* Zero bytes read, with success, is end-of-stream. A caller looping
     * until it reads nothing terminates; one told an error might retry. */
    if (ARG(3)) w32_write(w, ARG(3), 4, 0);
    RET(1);
}
static void wi_InternetGetConnectedState(w32 *w) {
    if (ARG(0)) w32_write(w, ARG(0), 4, 0);
    RET(0);                                         /* not connected */
}
static void wi_InternetSetOptionA(w32 *w) { (void)w; RET(1); }
static void wi_InternetCanonicalizeUrlA(w32 *w) {
    /* Copying the URL through unchanged is a correct canonicalisation of a
     * URL that is already canonical, and a program only uses the result to
     * pass back to a call that is going to fail anyway. */
    const char *s = ARG(0) ? w32_str(w, ARG(0)) : "";
    uint64_t out = ARG(1), lenp = ARG(2);
    uint32_t cap = lenp ? (uint32_t)w32_read(w, lenp, 4) : 0;
    size_t n = strlen(s);
    if (lenp) w32_write(w, lenp, 4, (uint32_t)n + 1);
    if (!out || cap <= n) { w32_set_last_error(w, 122); RET(0); return; }
    for (size_t i = 0; i <= n; i++) w32_write(w, out + i, 1, (uint8_t)s[i]);
    RET(1);
}
static void wi_InternetCrackUrlA(w32 *w) { w32_set_last_error(w, 12005); RET(0); }  /* ERROR_INTERNET_INVALID_URL */

#define N(n, a) { #n, a, 0, wi_##n, 0 }
const w32_api w32_wininet[] = {
    N(InternetOpenA, 5), N(InternetOpenW, 5),
    N(InternetConnectA, 8), N(InternetConnectW, 8),
    N(InternetCloseHandle, 1),
    N(HttpOpenRequestA, 8), N(HttpOpenRequestW, 8),
    N(HttpSendRequestA, 5), N(HttpSendRequestW, 5), N(HttpQueryInfoA, 5),
    N(InternetReadFile, 4), N(InternetGetConnectedState, 2),
    N(InternetSetOptionA, 4), N(InternetCanonicalizeUrlA, 4), N(InternetCrackUrlA, 4),
    { 0, 0, 0, 0, 0 },
};
#undef N
