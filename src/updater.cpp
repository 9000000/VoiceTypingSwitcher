#include "app.h"
#include <winhttp.h>
#include <cstdio>
#include <cstdlib>

// ---------------- SemVer & GitHub JSON parsing ----------------
static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

bool ParseSemVer(const wchar_t* tag, int& maj, int& min, int& pat) {
    if (!tag) return false;
    while (*tag == L' ' || *tag == L'v' || *tag == L'V') ++tag;
    if (*tag < L'0' || *tag > L'9') return false;
    wchar_t* end = nullptr;
    maj = (int)wcstol(tag, &end, 10);
    min = pat = 0;
    if (*end == L'.') {
        tag = end + 1;
        if (*tag < L'0' || *tag > L'9') return false;
        min = (int)wcstol(tag, &end, 10);
        if (*end == L'.') {
            tag = end + 1;
            if (*tag < L'0' || *tag > L'9') return false;
            pat = (int)wcstol(tag, &end, 10);
        }
    }
    return true;
}

bool IsNewerVersion(const wchar_t* remoteTag, int curMaj, int curMin, int curPat) {
    int rMaj = 0, rMin = 0, rPat = 0;
    if (!ParseSemVer(remoteTag, rMaj, rMin, rPat)) return false;
    if (rMaj != curMaj) return rMaj > curMaj;
    if (rMin != curMin) return rMin > curMin;
    return rPat > curPat;
}

// Extracts a JSON string value immediately following a key like `"tag_name"`.
static bool FindJsonString(const std::string& json, const char* key, size_t startPos,
                           std::string& outVal, size_t* outNextPos = nullptr) {
    std::string pat = std::string("\"") + key + "\"";
    size_t pos = startPos;
    while ((pos = json.find(pat, pos)) != std::string::npos) {
        size_t i = pos + pat.size();
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) ++i;
        if (i >= json.size() || json[i] != ':') { pos = i; continue; }
        ++i;
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) ++i;
        if (i >= json.size() || json[i] != '"') { pos = i; continue; }
        ++i;
        std::string val;
        while (i < json.size() && json[i] != '"') {
            if (json[i] == '\\' && i + 1 < json.size()) {
                char esc = json[++i];
                if (esc == 'n') val.push_back('\n');
                else if (esc == 'r') val.push_back('\r');
                else if (esc == 't') val.push_back('\t');
                else val.push_back(esc);
            } else {
                val.push_back(json[i]);
            }
            ++i;
        }
        if (i < json.size() && json[i] == '"') {
            outVal = val;
            if (outNextPos) *outNextPos = i + 1;
            return true;
        }
        return false;
    }
    return false;
}

static bool EndsWithExe(const std::string& url) {
    size_t end = url.find('?');
    if (end == std::string::npos) end = url.size();
    if (end < 4) return false;
    return _strnicmp(url.c_str() + end - 4, ".exe", 4) == 0;
}

bool ParseReleaseJson(const std::string& json, UpdateInfo& out) {
    std::string tag;
    if (!FindJsonString(json, "tag_name", 0, tag) || tag.empty()) return false;
    out.tag = Utf8ToWide(tag);

    // Find top-level release html_url (contains "/releases/"), avoiding author/uploader html_url.
    size_t pos = 0;
    std::string url;
    while (FindJsonString(json, "html_url", pos, url, &pos)) {
        if (url.find("/releases/") != std::string::npos) {
            out.htmlUrl = Utf8ToWide(url);
            break;
        }
    }
    if (out.htmlUrl.empty()) out.htmlUrl = APP_GITHUB_URL L"/releases";

    // Find .exe asset download URL (prefer one containing "VoiceTypingSwitcher").
    pos = 0;
    std::string dl, bestDl;
    while (FindJsonString(json, "browser_download_url", pos, dl, &pos)) {
        if (!EndsWithExe(dl)) continue;
        if (bestDl.empty() || dl.find("VoiceTypingSwitcher") != std::string::npos) {
            bestDl = dl;
            if (dl.find("VoiceTypingSwitcher") != std::string::npos) break;
        }
    }
    out.exeUrl = Utf8ToWide(bestDl);
    return true;
}

// ---------------- WinHTTP helpers ----------------
struct HttpReq {
    HINTERNET ses = nullptr, con = nullptr, req = nullptr;
    ~HttpReq() {
        if (req) WinHttpCloseHandle(req);
        if (con) WinHttpCloseHandle(con);
        if (ses) WinHttpCloseHandle(ses);
    }
};

static bool OpenUrl(HttpReq& h, const std::wstring& url, const wchar_t* headers, DWORD& outStatus,
                    DWORD& outContentLen) {
    outStatus = 0;
    outContentLen = 0;

    URL_COMPONENTS uc{};
    wchar_t host[256] = L"", path[2048] = L"";
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;
    uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = ARRAYSIZE(path);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;

    h.ses = WinHttpOpen(L"VoiceTypingSwitcher/" APP_VER_WSTR, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h.ses) {
        h.ses = WinHttpOpen(L"VoiceTypingSwitcher/" APP_VER_WSTR, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!h.ses) return false;

    WinHttpSetTimeouts(h.ses, 10000, 10000, 15000, 30000);
    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(h.ses, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));

    h.con = WinHttpConnect(h.ses, host, uc.nPort, 0);
    if (!h.con) return false;

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    h.req = WinHttpOpenRequest(h.con, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                               WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!h.req) return false;

    if (!WinHttpSendRequest(h.req, headers ? headers : WINHTTP_NO_ADDITIONAL_HEADERS,
                            headers ? (DWORD)-1L : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
        return false;
    if (!WinHttpReceiveResponse(h.req, nullptr)) return false;

    DWORD sz = sizeof(outStatus);
    WinHttpQueryHeaders(h.req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &outStatus, &sz, WINHTTP_NO_HEADER_INDEX);
    sz = sizeof(outContentLen);
    if (!WinHttpQueryHeaders(h.req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &outContentLen, &sz, WINHTTP_NO_HEADER_INDEX))
        outContentLen = 0;
    return true;
}

static bool VerifyPeFile(const wchar_t* path) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = false;
    if (GetFileSizeEx(f, &sz) && sz.QuadPart >= 32768 && sz.QuadPart <= 50 * 1024 * 1024) {
        IMAGE_DOS_HEADER dos{};
        DWORD read = 0;
        if (ReadFile(f, &dos, sizeof(dos), &read, nullptr) && read == sizeof(dos) &&
            dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew >= (LONG)sizeof(dos) &&
            dos.e_lfanew < sz.QuadPart - 4) {
            SetFilePointer(f, dos.e_lfanew, nullptr, FILE_BEGIN);
            DWORD peSig = 0;
            if (ReadFile(f, &peSig, sizeof(peSig), &read, nullptr) && read == sizeof(peSig) &&
                peSig == IMAGE_NT_SIGNATURE) {
                ok = true;
            }
        }
    }
    CloseHandle(f);
    return ok;
}

// ---------------- Async check & download threads ----------------
static DWORD WINAPI CheckThread(LPVOID lp) {
    HWND wnd = (HWND)lp;
    auto* info = new UpdateInfo();
    std::wstring apiUrl = L"https://api.github.com/repos/" APP_GITHUB_OWNER L"/" APP_GITHUB_REPO L"/releases/latest";

    HttpReq h;
    DWORD status = 0, clen = 0;
    if (!OpenUrl(h, apiUrl, L"Accept: application/vnd.github+json\r\n", status, clen)) {
        info->status = UpdError;
        info->error = L"HTTP request failed";
    } else {
        info->httpStatus = status;
        if (status == 404) {
            info->status = UpdNoReleases;
            info->htmlUrl = APP_GITHUB_URL L"/releases";
        } else if (status != 200) {
            info->status = UpdError;
            wchar_t buf[32];
            wsprintfW(buf, L"HTTP %lu", status);
            info->error = buf;
        } else {
            std::string body;
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(h.req, &avail) && avail > 0) {
                if (body.size() + avail > 1024 * 1024) break;
                size_t cur = body.size();
                body.resize(cur + avail);
                DWORD got = 0;
                if (!WinHttpReadData(h.req, &body[cur], avail, &got)) break;
                body.resize(cur + got);
            }
            if (!ParseReleaseJson(body, *info)) {
                info->status = UpdError;
                info->error = L"Invalid GitHub release response";
            } else if (IsNewerVersion(info->tag.c_str(), APP_VER_MAJOR, APP_VER_MINOR, APP_VER_PATCH)) {
                info->status = UpdAvailable;
            } else {
                info->status = UpdUpToDate;
            }
        }
    }
    PostMessageW(wnd, WM_APP_UPDATE_CHECK, 0, (LPARAM)info);
    return 0;
}

struct DlParams {
    HWND wnd;
    std::wstring url;
};

static DWORD WINAPI DownloadThread(LPVOID lp) {
    auto* p = (DlParams*)lp;
    HWND wnd = p->wnd;
    std::wstring url = p->url;
    delete p;

    auto* res = new DownloadResult();
    wchar_t tempDir[MAX_PATH], tempFile[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tempDir) ||
        !GetTempFileNameW(tempDir, L"vts", 0, tempFile)) {
        res->error = L"Cannot create temporary file";
        PostMessageW(wnd, WM_APP_UPDATE_DL, 0, (LPARAM)res);
        return 0;
    }

    HttpReq h;
    DWORD status = 0, clen = 0;
    if (!OpenUrl(h, url, L"Accept: application/octet-stream\r\n", status, clen) || status != 200) {
        DeleteFileW(tempFile);
        wchar_t buf[48];
        wsprintfW(buf, L"Download failed (HTTP %lu)", status);
        res->error = buf;
        PostMessageW(wnd, WM_APP_UPDATE_DL, 0, (LPARAM)res);
        return 0;
    }

    HANDLE f = CreateFileW(tempFile, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        DeleteFileW(tempFile);
        res->error = L"Cannot write temporary file";
        PostMessageW(wnd, WM_APP_UPDATE_DL, 0, (LPARAM)res);
        return 0;
    }

    DWORD total = 0, avail = 0;
    bool writeOk = true;
    char buf[16384];
    while (WinHttpQueryDataAvailable(h.req, &avail) && avail > 0) {
        while (avail > 0) {
            DWORD chunk = avail > sizeof(buf) ? (DWORD)sizeof(buf) : avail;
            DWORD got = 0, written = 0;
            if (!WinHttpReadData(h.req, buf, chunk, &got) || got == 0) { writeOk = false; break; }
            if (!WriteFile(f, buf, got, &written, nullptr) || written != got) { writeOk = false; break; }
            total += got;
            avail -= got;
        }
        if (!writeOk) break;
    }
    CloseHandle(f);

    if (!writeOk || (clen > 0 && total != clen) || !VerifyPeFile(tempFile)) {
        DeleteFileW(tempFile);
        res->error = L"Downloaded file is incomplete or invalid";
        PostMessageW(wnd, WM_APP_UPDATE_DL, 0, (LPARAM)res);
        return 0;
    }

    res->ok = true;
    res->tempExePath = tempFile;
    PostMessageW(wnd, WM_APP_UPDATE_DL, 0, (LPARAM)res);
    return 0;
}

void StartUpdateCheckAsync(HWND notifyWnd) {
    HANDLE th = CreateThread(nullptr, 0, CheckThread, notifyWnd, 0, nullptr);
    if (th) CloseHandle(th);
}

void StartUpdateDownloadAsync(HWND notifyWnd, const std::wstring& exeUrl) {
    auto* p = new DlParams{notifyWnd, exeUrl};
    HANDLE th = CreateThread(nullptr, 0, DownloadThread, p, 0, nullptr);
    if (th) CloseHandle(th);
    else delete p;
}

bool ReplaceSelfAndPrepareRelaunch(const std::wstring& tempExePath, std::wstring& outExePath) {
    wchar_t cur[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, cur, MAX_PATH)) return false;
    std::wstring exe(cur);
    std::wstring old = exe + L".old";
    DeleteFileW(old.c_str());

    if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
    if (!MoveFileExW(tempExePath.c_str(), exe.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
        return false;
    }
    outExePath = exe;
    return true;
}

void CleanupOldBinary(const wchar_t* cmdLine) {
    if (cmdLine) {
        const wchar_t* p = wcsstr(cmdLine, L"--updated ");
        if (p) {
            DWORD oldPid = (DWORD)wcstoul(p + 10, nullptr, 10);
            if (oldPid) {
                HANDLE hp = OpenProcess(SYNCHRONIZE, FALSE, oldPid);
                if (hp) {
                    WaitForSingleObject(hp, 3000);
                    CloseHandle(hp);
                }
            }
        }
    }
    wchar_t cur[MAX_PATH];
    if (GetModuleFileNameW(nullptr, cur, MAX_PATH)) {
        std::wstring old = std::wstring(cur) + L".old";
        DeleteFileW(old.c_str());
    }
}
