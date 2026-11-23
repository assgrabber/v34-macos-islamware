#include <windows.h>
#include <winhttp.h>
#include "../src/cheat.h"

// WinHTTP lives in its own translation unit: wininet.h and winhttp.h redeclare
// URL_COMPONENTS / INTERNET_SCHEME and cannot be included side by side.

bool HttpGetWinHttpExt(const char* url, char* buf, int buf_size, int& out_len) {
    const char* p = url;
    bool https = !_strnicmp(p, "https://", 8);
    p += https ? 8 : 7;
    const char* slash = strchr(p, '/');
    if (!slash) return false;

    wchar_t host[256] = { 0 }, path[1024] = { 0 };
    MultiByteToWideChar(CP_UTF8, 0, p, (int)(slash - p), host, 255);
    MultiByteToWideChar(CP_UTF8, 0, slash, -1, path, 1023);

    HINTERNET session = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { Log("[serverlist] winhttp open-session err=%lu", GetLastError()); return false; }
    WinHttpSetTimeouts(session, 15000, 15000, 15000, 15000);

    HINTERNET conn = WinHttpConnect(session, host,
        https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    if (!req) {
        Log("[serverlist] winhttp connect err=%lu", GetLastError());
        if (conn) WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return false;
    }
    if (https) {
        DWORD sec = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                    SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(req, WINHTTP_OPTION_SECURITY_FLAGS, &sec, sizeof(sec));
    }

    bool ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr);
    if (!ok) Log("[serverlist] winhttp send err=%lu", GetLastError());

    out_len = 0;
    if (ok) {
        DWORD read = 0;
        while (out_len < buf_size - 1 && WinHttpReadData(req, buf + out_len, 16384, &read) && read > 0)
            out_len += (int)read;
    }
    buf[out_len] = '\0';
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return out_len > 0;
}