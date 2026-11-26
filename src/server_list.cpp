#include "cheat.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <wininet.h>
#include <process.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

// ---------------------------------------------------------------------------
// Community server list fix.
//
// The Steam master server (hl2master.steampowered.com:27011) was retired by
// Valve — the hostname now resolves to a dead internal CNAME, so the emu
// (Goldberg / gbe_fork) has nothing to query and the Internet tab stays
// empty.  gbe_fork reads <save>/7/remote/serverbrowser.txt as the Internet
// list (one "ip:port" per line) and A2S-queries every entry itself, so the
// fix is: fetch a public server index over HTTP, pre-validate each server
// with A2S_INFO (drops dead hosts and protocol-mismatched ones, e.g. v34
// servers that a 9540945 client can't join) and write that file.
// ---------------------------------------------------------------------------

// Status shared with the settings page.  volatile int is fine: single
// writer thread, UI only reads.
volatile LONG sl_state = 0;      // 0 idle, 1 running, 2 done, 3 failed
static volatile LONG sl_found = 0;   // entries scraped
static volatile LONG sl_alive = 0;   // entries that answered A2S
static volatile LONG sl_kept = 0;    // entries written after protocol filter
static DWORD sl_last_tick = 0;

// A2S_INFO protocol byte: 7 = pre-2013 (v34-era) builds, 17 = Source 2013+
// (every current CS:S server).  The engine's internal connect protocol is a
// different number (24), so the filter uses the A2S value.
static int ClientProtocol() {
    return 17;
}

// ---------------------------------------------------------------------------
// HTTP: pull gametracker's CS:S index, return ip:port pairs.
// ---------------------------------------------------------------------------

static bool HttpGetWininet(const char* url, char* buf, int buf_size, int& out_len) {
    HINTERNET inet = InternetOpenA("Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
                                   INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!inet) { Log("[serverlist] wininet open-session err=%lu", GetLastError()); return false; }
    DWORD t = 15000;
    InternetSetOptionA(inet, INTERNET_OPTION_CONNECT_TIMEOUT, &t, sizeof(t));
    InternetSetOptionA(inet, INTERNET_OPTION_RECEIVE_TIMEOUT, &t, sizeof(t));

    HINTERNET h = InternetOpenUrlA(inet, url, nullptr, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!h) {
        Log("[serverlist] wininet GET '%s' err=%lu", url, GetLastError());
        InternetCloseHandle(inet);
        return false;
    }

    out_len = 0;
    DWORD read = 0;
    while (out_len < buf_size - 1 && InternetReadFile(h, buf + out_len, 16384, &read) && read > 0)
        out_len += (int)read;
    buf[out_len] = '\0';
    InternetCloseHandle(h);
    InternetCloseHandle(inet);
    return out_len > 0;
}

bool HttpGetWinHttpExt(const char* url, char* buf, int buf_size, int& out_len);

static bool HttpGet(const char* url, char* buf, int buf_size, int& out_len) {
    if (HttpGetWininet(url, buf, buf_size, out_len)) return true;
    Log("[serverlist] wininet failed, trying winhttp");
    return HttpGetWinHttpExt(url, buf, buf_size, out_len);
}

// Collect unique "ip:port" from gametracker HTML.  Returns count added.
static int ParseServerLinks(const char* html, char entries[][32], int max_entries, int& n_entries) {
    int added = 0;
    const char* p = html;
    while (p && (p = strstr(p, "server_info/"))) {
        unsigned a, b, c, d, port;
        if (sscanf(p, "server_info/%u.%u.%u.%u:%u/", &a, &b, &c, &d, &port) == 5
            && a <= 255 && b <= 255 && c <= 255 && d <= 255 && port > 0 && port <= 65535) {
            char line[32];
            snprintf(line, sizeof(line), "%u.%u.%u.%u:%u", a, b, c, d, port);
            bool dup = false;
            for (int i = 0; i < n_entries; i++) {
                if (!strcmp(entries[i], line)) { dup = true; break; }
            }
            if (!dup && n_entries < max_entries) {
                snprintf(entries[n_entries++], 32, "%s", line);
                added++;
            }
        }
        p += 12;
    }
    return added;
}

// ---------------------------------------------------------------------------
// UDP: A2S_INFO probe.  Returns true when the server answered with a source
// info reply; protocol / players / maxplayers out.
// ---------------------------------------------------------------------------

static bool A2SInfo(const char* ip, unsigned short port,
                    int& protocol, int& players, int& maxplayers) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        Log("[serverlist] socket() fail err=%d", WSAGetLastError());
        return false;
    }
    DWORD tmo = 1500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip);

    char req[32];
    int req_len = snprintf(req, sizeof(req), "%c%c%c%cTSource Engine Query%c",
                           (char)0xFF, (char)0xFF, (char)0xFF, (char)0xFF, 0);

    char reply[1500];
    bool got = false;
    for (int attempt = 0; attempt < 2 && !got; attempt++) {
        if (sendto(s, req, req_len, 0, (sockaddr*)&addr, sizeof(addr)) < 0) break;
        int n = recvfrom(s, reply, sizeof(reply), 0, nullptr, nullptr);
        if (n <= 5) continue;

        const char* p = reply;
        if (memcmp(p, "\xff\xff\xff\xff", 4) != 0) continue;   // split/bzip replies we don't want
        if (p[4] == 'A') {
            // modern servers answer with a challenge — resend it appended
            if (req_len + 4 > (int)sizeof(req)) break;
            memcpy(req + req_len, p + 5, 4);
            req_len += 4;
            if (sendto(s, req, req_len, 0, (sockaddr*)&addr, sizeof(addr)) < 0) break;
            n = recvfrom(s, reply, sizeof(reply), 0, nullptr, nullptr);
            if (n <= 5 || memcmp(reply, "\xff\xff\xff\xff", 4) != 0) continue;
            p = reply;
        }
        if (p[4] != 'I') continue;                             // not a source info reply

        // type, protocol, name\0, map\0, folder\0, game\0, appid(2), players(1), max(1)
        protocol = (unsigned char)p[5];
        const char* q = p + 6;
        for (int f = 0; f < 4 && *q; f++) {                    // skip 4 NUL-terminated strings
            q += strlen(q) + 1;
        }
        if (q + 4 <= reply + n) {
            players = (unsigned char)q[2];
            maxplayers = (unsigned char)q[3];
        }
        got = true;
    }
    closesocket(s);
    return got;
}

// ---------------------------------------------------------------------------
// Refresh worker.
// ---------------------------------------------------------------------------

static void AppendStatusLine(const char* what) {
    Log("[serverlist] %s", what);
}

static DWORD WINAPI RefreshThread(LPVOID) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        sl_state = 3;
        return 0;
    }

    const int MAX_ENTRIES = 300;
    static char entries[MAX_ENTRIES][32];
    int n_entries = 0;

    // page 1 contains all links we need per page; start=0,25,... gametracker
    // paginates its css index, walk until a page adds nothing new (8 pages max)
    int found = 0;
    for (int page = 0; page < 4; page++) {
        char url[160];
        snprintf(url, sizeof(url),
                 "https://www.gametracker.com/search/css/?searchipp=50&searchpge=%d", page + 1);
        static char html[900000];
        int len = 0;
        if (!HttpGet(url, html, (int)sizeof(html), len)) break;
        int added = ParseServerLinks(html, entries, MAX_ENTRIES, n_entries);
        found += added;
        Log("[serverlist] page %d: +%d (total %d)", page, added, n_entries);
        if (added == 0) break;
    }
    sl_found = found;
    if (n_entries == 0) {
        AppendStatusLine("fetch failed: no servers scraped");
        sl_state = 3;
        WSACleanup();
        return 0;
    }

    int client_proto = ClientProtocol();
    Log("[serverlist] client protocol %d, probing %d entries", client_proto, n_entries);

    // probe + protocol filter, parallel across worker threads
    static char kept[MAX_ENTRIES][32];
    static int n_kept = 0, alive = 0;
    static CRITICAL_SECTION probe_cs;
    InitializeCriticalSection(&probe_cs);
    n_kept = 0; alive = 0;

    static volatile LONG next_index = 0;
    next_index = 0;

    struct ProbeCtx { int n_entries; int client_proto; };
    static ProbeCtx ctx;
    ctx.n_entries = n_entries;
    ctx.client_proto = client_proto;

    static HANDLE workers[24];
    auto worker = [](LPVOID lp) WINAPI -> DWORD {
        ProbeCtx* c = (ProbeCtx*)lp;
        for (;;) {
            LONG i = InterlockedIncrement(&next_index) - 1;
            if (i >= c->n_entries) break;
            unsigned a, b, d1, d2, port;
            if (sscanf(entries[i], "%u.%u.%u.%u:%u", &a, &b, &d1, &d2, &port) != 5)
                continue;
            char ipbuf[20];
            snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u", a, b, d1, d2);
            int proto = -1, players = -1, maxp = -1;
            if (A2SInfo(ipbuf, (unsigned short)port, proto, players, maxp)) {
                EnterCriticalSection(&probe_cs);
                alive++;
                if (proto == c->client_proto && n_kept < MAX_ENTRIES)
                    snprintf(kept[n_kept++], 32, "%s", entries[i]);
                LeaveCriticalSection(&probe_cs);
            }
            InterlockedExchange(&sl_alive, alive);
            InterlockedExchange(&sl_kept, n_kept);
        }
        return 0;
    };
    int nthreads = 24;
    if (n_entries < nthreads) nthreads = n_entries;
    for (int t = 0; t < nthreads; t++)
        workers[t] = CreateThread(nullptr, 0, worker, &ctx, 0, nullptr);
    for (int t = 0; t < nthreads; t++)
        if (workers[t]) { WaitForSingleObject(workers[t], INFINITE); CloseHandle(workers[t]); }
    DeleteCriticalSection(&probe_cs);
    int final_alive = alive;
    Log("[serverlist] probed: %d alive, %d protocol-matching", final_alive, n_kept);

    if (n_kept == 0) {
        AppendStatusLine("no compatible servers found (client protocol is current)");
        sl_state = 3;
        WSACleanup();
        return 0;
    }

    // write <APPDATA>\GSE Saves\7\remote\serverbrowser.txt — the exact file
    // gbe_fork reads for the Internet tab.  Merge with what is already there
    // so manual favorites the player put in the file survive refreshes.
    char appdata[MAX_PATH] = { 0 };
    GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
    if (!appdata[0]) {
        sl_state = 3;
        WSACleanup();
        return 0;
    }
    char dir1[MAX_PATH], dir2[MAX_PATH], dir3[MAX_PATH];
    snprintf(dir1, sizeof(dir1), "%s\\GSE Saves", appdata);
    snprintf(dir2, sizeof(dir2), "%s\\7", dir1);
    snprintf(dir3, sizeof(dir3), "%s\\remote", dir2);
    CreateDirectoryA(dir1, nullptr);
    CreateDirectoryA(dir2, nullptr);
    CreateDirectoryA(dir3, nullptr);
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\serverbrowser.txt", dir3);

    // existing entries first (they stay at the top of the list)
    char merged[MAX_ENTRIES + 64][32];
    int n_merged = 0;
    FILE* old = fopen(path, "rb");
    if (old) {
        char line[64];
        while (n_merged < MAX_ENTRIES + 60 && fgets(line, sizeof(line), old)) {
            unsigned a, b, c, d, port;
            if (sscanf(line, "%u.%u.%u.%u:%u", &a, &b, &c, &d, &port) == 5) {
                char clean[32];
                snprintf(clean, sizeof(clean), "%u.%u.%u.%u:%u", a, b, c, d, port);
                bool dup = false;
                for (int i = 0; i < n_merged; i++)
                    if (!strcmp(merged[i], clean)) { dup = true; break; }
                if (!dup) snprintf(merged[n_merged++], 32, "%s", clean);
            }
        }
        fclose(old);
    }
    for (int i = 0; i < n_kept && n_merged < MAX_ENTRIES + 64; i++) {
        bool dup = false;
        for (int j = 0; j < n_merged; j++)
            if (!strcmp(merged[j], kept[i])) { dup = true; break; }
        if (!dup) snprintf(merged[n_merged++], 32, "%s", kept[i]);
    }

    FILE* f = fopen(path, "wb");
    if (!f) {
        Log("[serverlist] cannot write %s", path);
        sl_state = 3;
        WSACleanup();
        return 0;
    }
    for (int i = 0; i < n_merged; i++) fprintf(f, "%s\n", merged[i]);
    fclose(f);

    Log("[serverlist] wrote %d servers to %s", n_merged, path);
    sl_kept = n_merged;
    sl_last_tick = GetTickCount();
    sl_state = 2;
    WSACleanup();
    return 0;
}

// Diagnostic used by the cmd channel's "net-test" line: probes DNS, UDP and
// TCP from inside the game process (Wine), logging each result.
void RunNetTest() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    addrinfo hints {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int gai = getaddrinfo("www.gametracker.com", "443", &hints, &res);
    Log("[nettest] dns gametracker: gai=%d wsa=%d", gai, WSAGetLastError());
    if (res) freeaddrinfo(res);

    // UDP A2S to the community server
    SOCKET u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (u != INVALID_SOCKET) {
        DWORD tmo = 4000;
        setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
        sockaddr_in a {};
        a.sin_family = AF_INET;
        a.sin_port = htons(9999);
        a.sin_addr.s_addr = inet_addr("46.174.49.149");
        const char req[] = "\xFF\xFF\xFF\xFFTSource Engine Query";
        int sent = sendto(u, req, (int)strlen(req) + 1, 0, (sockaddr*)&a, sizeof(a));
        char reply[512];
        int rn = recvfrom(u, reply, sizeof(reply), 0, nullptr, nullptr);
        Log("[nettest] udp a2s sent=%d recv=%d err=%d", sent, rn, WSAGetLastError());
        closesocket(u);
    }

    // UDP DNS to a WAN resolver
    SOCKET d = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (d != INVALID_SOCKET) {
        DWORD tmo = 4000;
        setsockopt(d, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
        sockaddr_in a {};
        a.sin_family = AF_INET;
        a.sin_port = htons(53);
        a.sin_addr.s_addr = inet_addr("8.8.8.8");
        char q[] = "\x00\x01\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00\x07example\x03com\x00\x00\x01\x00\x01";
        int sent = sendto(d, q, sizeof(q) - 1, 0, (sockaddr*)&a, sizeof(a));
        char rep[512];
        int rn = recvfrom(d, rep, sizeof(rep), 0, nullptr, nullptr);
        Log("[nettest] udp dns 8.8.8.8 sent=%d recv=%d err=%d", sent, rn, WSAGetLastError());
        closesocket(d);
    }

    // resolve gametracker and try HTTPS directly
    char gt_ip[32] = "0.0.0.0";
    addrinfo gh {}, *gr = nullptr;
    gh.ai_family = AF_INET;
    gh.ai_socktype = SOCK_STREAM;
    if (getaddrinfo("www.gametracker.com", "443", &gh, &gr) == 0 && gr) {
        sockaddr_in* sa = (sockaddr_in*)gr->ai_addr;
        strncpy(gt_ip, inet_ntoa(sa->sin_addr), sizeof(gt_ip) - 1);
        freeaddrinfo(gr);
    }
    Log("[nettest] gametracker ip %s", gt_ip);

    const char* targets[][2] = { { "192.168.0.1", "80" }, { gt_ip, "443" },
                                 { "140.82.112.4", "443" }, { "5.9.124.114", "27015" } };

    // test whether explicitly binding the source address fixes WAN (some Wine
    // builds on multi-homed Macs pick the wrong interface)
    {
        SOCKET b = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in local {};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = inet_addr("192.168.0.101");
        local.sin_port = 0;
        int br = bind(b, (sockaddr*)&local, sizeof(local));
        u_long nb = 1;
        ioctlsocket(b, FIONBIO, &nb);
        sockaddr_in a {};
        a.sin_family = AF_INET;
        a.sin_port = htons(443);
        a.sin_addr.s_addr = inet_addr("140.82.112.4");
        int cr = connect(b, (sockaddr*)&a, sizeof(a));
        fd_set wf; FD_ZERO(&wf); FD_SET(b, &wf);
        timeval tv { 8, 0 };
        int sel = select(0, nullptr, &wf, nullptr, &tv);
        Log("[nettest] bound tcp github bind=%d connect=%d select=%d err=%d", br, cr, sel, WSAGetLastError());
        closesocket(b);
    }
    for (auto& tg : targets) {
        SOCKET t = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (t == INVALID_SOCKET) continue;
        u_long nb = 1;
        ioctlsocket(t, FIONBIO, &nb);
        sockaddr_in a {};
        a.sin_family = AF_INET;
        a.sin_port = htons((u_short)atoi(tg[1]));
        a.sin_addr.s_addr = inet_addr(tg[0]);
        connect(t, (sockaddr*)&a, sizeof(a));
        fd_set wf; FD_ZERO(&wf); FD_SET(t, &wf);
        timeval tv { 8, 0 };
        int sel = select(0, nullptr, &wf, nullptr, &tv);
        int err = 0; int elen = sizeof(err);
        getsockopt(t, SOL_SOCKET, SO_ERROR, (char*)&err, &elen);
        sockaddr_in local {};
        int llen = sizeof(local);
        getsockname(t, (sockaddr*)&local, &llen);
        Log("[nettest] tcp %s:%s select=%d so_error=%d local=%s", tg[0], tg[1], sel, err,
            inet_ntoa(local.sin_addr));
        closesocket(t);
    }
    Log("[nettest] done");
}

bool StartServerListRefresh(const char* reason) {
    if (InterlockedCompareExchange(&sl_state, 1, 0) != 0)
        return false;   // already running
    Log("[serverlist] refresh started (%s)", reason ? reason : "manual");
    HANDLE h = CreateThread(nullptr, 0, RefreshThread, nullptr, 0, nullptr);
    if (!h) {
        sl_state = 3;
        return false;
    }
    CloseHandle(h);
    return true;
}

// Text for the settings page, two short lines.
void ServerListStatus(char* buf, int buf_size) {
    switch (sl_state) {
        case 1:
            snprintf(buf, buf_size, "fetching server list...  found %d / alive %d", (int)sl_found, (int)sl_alive);
            break;
        case 2: {
            DWORD age = (GetTickCount() - sl_last_tick) / 1000;
            if (age < 90) snprintf(buf, buf_size, "done: %d servers listed just now", (int)sl_kept);
            else snprintf(buf, buf_size, "done: %d servers listed (%d min ago)", (int)sl_kept, age / 60);
            break;
        }
        case 3:
            snprintf(buf, buf_size, "refresh failed — check islamware_css.log");
            break;
        default:
            snprintf(buf, buf_size, "idle — open the Internet tab after a refresh");
            break;
    }
}
