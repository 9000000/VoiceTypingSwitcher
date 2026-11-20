#include "app.h"
#include <dwmapi.h>
#include <tlhelp32.h>
#include <uiautomation.h>
#include <cstdarg>
#include <cstdio>

// ---------------- Diagnostic session logger (%TEMP%\VoiceTypingSwitcher.log) ----------------
static DWORD s_logStart = 0;

static void GetLogPath(wchar_t* buf, DWORD cap) {
    DWORD n = GetTempPathW(cap, buf);
    if (n == 0 || n + 32 >= cap) { buf[0] = 0; return; }
    lstrcatW(buf, L"VoiceTypingSwitcher.log");
}

void ResetSessionLog() {
    s_logStart = GetTickCount();
    wchar_t path[MAX_PATH];
    GetLogPath(path, MAX_PATH);
    if (!path[0]) return;
    FILE* f = _wfopen(path, L"w, ccs=UTF-8");
    if (f) fclose(f);
}

void LogSession(const wchar_t* fmt, ...) {
    wchar_t path[MAX_PATH];
    GetLogPath(path, MAX_PATH);
    if (!path[0]) return;
    FILE* f = _wfopen(path, L"a, ccs=UTF-8");
    if (!f) return;
    fwprintf(f, L"[+%5lums] ", GetTickCount() - s_logStart);
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(f, fmt, ap);
    va_end(ap);
    fputwc(L'\n', f);
    fclose(f);
}

// ---------------- Process & Win32 window helpers ----------------
static std::vector<DWORD> HostPids() {
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (!lstrcmpiW(pe.szExeFile, L"TextInputHost.exe")) pids.push_back(pe.th32ProcessID);
    CloseHandle(snap);
    return pids;
}

static bool IsHostWindow(HWND h, const std::vector<DWORD>& pids) {
    if (!h) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    for (DWORD p : pids)
        if (p == pid) return true;
    return false;
}

static bool IsShown(HWND h) {
    if (!IsWindowVisible(h)) return false;
    DWORD cloaked = 0;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return false;
    RECT r;
    return GetWindowRect(h, &r) && r.right - r.left > 0 && r.bottom - r.top > 0;
}

static bool IsFloatingRect(const RECT& r) {
    int w = r.right - r.left, h = r.bottom - r.top;
    return w >= 40 && h >= 20 && w <= 900 && h <= 500;
}

// Collects visible TextInputHost top-level HWNDs and child HWNDs inside ApplicationFrame / topmost hosts.
// Uses FindWindowExW because EnumWindows skips non-desktop Z-order bands (ZBID_IMMERSIVE_*).
static std::vector<HWND> VisibleHostHwnds(const std::vector<DWORD>& pids) {
    struct Ctx { const std::vector<DWORD>* pids; std::vector<HWND> out; } ctx{&pids, {}};
    if (pids.empty()) return {};
    HWND h = nullptr;
    for (int guard = 0; guard < 2000 && (h = FindWindowExW(nullptr, h, nullptr, nullptr)) != nullptr; ++guard) {
        if (!IsShown(h)) continue;
        if (IsHostWindow(h, pids)) {
            ctx.out.push_back(h);
            continue;
        }
        LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
        wchar_t cls[64] = L"";
        GetClassNameW(h, cls, 64);
        if ((ex & WS_EX_TOPMOST) || !lstrcmpW(cls, L"ApplicationFrameWindow")) {
            EnumChildWindows(h, [](HWND ch, LPARAM lp2) -> BOOL {
                auto* c2 = (Ctx*)lp2;
                if (IsHostWindow(ch, *c2->pids) && IsShown(ch)) c2->out.push_back(ch);
                return TRUE;
            }, (LPARAM)&ctx);
        }
    }
    return ctx.out;
}

// ---------------- UI Automation ----------------
static IUIAutomation* s_uia = nullptr;
static bool s_comInit = false;

bool InitVoiceBar() {
    if (s_uia) return true;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) s_comInit = true;
    else if (hr != RPC_E_CHANGED_MODE) return false;
    hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_IUIAutomation, (void**)&s_uia);
    return SUCCEEDED(hr) && s_uia != nullptr;
}

void CleanupVoiceBar() {
    if (s_uia) { s_uia->Release(); s_uia = nullptr; }
    if (s_comInit) { CoUninitialize(); s_comInit = false; }
}

static IUIAutomationCondition* MakeIntCond(PROPERTYID prop, int val) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = val;
    IUIAutomationCondition* c = nullptr;
    s_uia->CreatePropertyCondition(prop, v, &c);
    return c;
}

static IUIAutomationCondition* MakeBoolCond(PROPERTYID prop, BOOL val) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = val ? VARIANT_TRUE : VARIANT_FALSE;
    IUIAutomationCondition* c = nullptr;
    s_uia->CreatePropertyCondition(prop, v, &c);
    return c;
}

static IUIAutomationCondition* BuildControlCond() {
    IUIAutomationCondition* cBtn = MakeIntCond(UIA_ControlTypePropertyId, UIA_ButtonControlTypeId);
    IUIAutomationCondition* cTxt = MakeIntCond(UIA_ControlTypePropertyId, UIA_TextControlTypeId);
    IUIAutomationCondition* cOn = MakeBoolCond(UIA_IsOffscreenPropertyId, FALSE);
    IUIAutomationCondition* cOr = nullptr;
    IUIAutomationCondition* cAnd = nullptr;
    if (cBtn && cTxt) s_uia->CreateOrCondition(cBtn, cTxt, &cOr);
    if (cOr && cOn) s_uia->CreateAndCondition(cOr, cOn, &cAnd);
    if (cBtn) cBtn->Release();
    if (cTxt) cTxt->Release();
    if (cOn) cOn->Release();
    if (cOr) cOr->Release();
    return cAnd;
}

static void AccumulateVisibleControls(IUIAutomationElement* container, IUIAutomationCondition* cond,
                                      RECT& box, bool& any) {
    IUIAutomationElementArray* arr = nullptr;
    if (FAILED(container->FindAll(TreeScope_Descendants, cond, &arr)) || !arr) return;
    int len = 0;
    arr->get_Length(&len);
    for (int i = 0; i < len; ++i) {
        IUIAutomationElement* el = nullptr;
        if (FAILED(arr->GetElement(i, &el)) || !el) continue;
        BOOL off = TRUE;
        RECT r{};
        if (SUCCEEDED(el->get_CurrentIsOffscreen(&off)) && !off &&
            SUCCEEDED(el->get_CurrentBoundingRectangle(&r))) {
            int w = r.right - r.left, h = r.bottom - r.top;
            if (w > 0 && h > 0 && w <= 900 && h <= 500) {
                if (!any) { box = r; any = true; }
                else {
                    if (r.left < box.left) box.left = r.left;
                    if (r.top < box.top) box.top = r.top;
                    if (r.right > box.right) box.right = r.right;
                    if (r.bottom > box.bottom) box.bottom = r.bottom;
                }
            }
        }
        el->Release();
    }
    arr->Release();
}

VoiceBarInfo QueryVoiceBar() {
    VoiceBarInfo info;
    std::vector<DWORD> pids = HostPids();
    if (pids.empty()) return info;

    std::vector<HWND> hwnds = VisibleHostHwnds(pids);

    // 1. Inspect UIA elements belonging to TextInputHost (or top-level "Voice typing" shell frame).
    if (InitVoiceBar()) {
        IUIAutomationElement* root = nullptr;
        if (SUCCEEDED(s_uia->GetRootElement(&root)) && root) {
            VARIANT vName;
            VariantInit(&vName);
            vName.vt = VT_BSTR;
            vName.bstrVal = SysAllocString(L"Voice typing");
            if (vName.bstrVal) {
                IUIAutomationCondition* cName = nullptr;
                if (SUCCEEDED(s_uia->CreatePropertyCondition(UIA_NamePropertyId, vName, &cName)) && cName) {
                    IUIAutomationElement* vtEl = nullptr;
                    if (SUCCEEDED(root->FindFirst(TreeScope_Children, cName, &vtEl)) && vtEl) {
                        BOOL off = TRUE;
                        RECT r{};
                        if (SUCCEEDED(vtEl->get_CurrentIsOffscreen(&off)) && !off &&
                            SUCCEEDED(vtEl->get_CurrentBoundingRectangle(&r)) && IsFloatingRect(r)) {
                            vtEl->Release();
                            cName->Release();
                            VariantClear(&vName);
                            root->Release();
                            info.visible = true;
                            info.rect = r;
                            return info;
                        }
                        vtEl->Release();
                    }
                    cName->Release();
                }
            }
            VariantClear(&vName);
            root->Release();
        }

        IUIAutomationCondition* cond = BuildControlCond();
        if (cond) {
            RECT box{};
            bool any = false;

            // Check visible host HWNDs directly first (top-level and ApplicationFrame children).
            for (HWND h : hwnds) {
                IUIAutomationElement* el = nullptr;
                if (SUCCEEDED(s_uia->ElementFromHandle(h, &el)) && el) {
                    AccumulateVisibleControls(el, cond, box, any);
                    el->Release();
                }
            }

            // Also check desktop children by TextInputHost PID in case HWND wasn't caught above.
            if (!any) {
                IUIAutomationElement* root = nullptr;
                if (SUCCEEDED(s_uia->GetRootElement(&root)) && root) {
                    for (DWORD pid : pids) {
                        IUIAutomationCondition* cPid = MakeIntCond(UIA_ProcessIdPropertyId, (int)pid);
                        if (!cPid) continue;
                        IUIAutomationElementArray* els = nullptr;
                        if (SUCCEEDED(root->FindAll(TreeScope_Children, cPid, &els)) && els) {
                            int len = 0;
                            els->get_Length(&len);
                            for (int i = 0; i < len; ++i) {
                                IUIAutomationElement* el = nullptr;
                                if (SUCCEEDED(els->GetElement(i, &el)) && el) {
                                    AccumulateVisibleControls(el, cond, box, any);
                                    el->Release();
                                }
                            }
                            els->Release();
                        }
                        cPid->Release();
                    }
                    root->Release();
                }
            }

            cond->Release();
            if (any) {
                InflateRect(&box, 36, 36);  // cover rounded toolbar padding, border, and grabber
                info.visible = true;
                info.rect = box;
                return info;
            }
        }
    }

    // 2. Fallback: compact floating visible TextInputHost window (ignore fullscreen host overlays).
    for (HWND h : hwnds) {
        RECT r{};
        if (GetWindowRect(h, &r) && IsFloatingRect(r)) {
            info.visible = true;
            info.rect = r;
            return info;
        }
    }
    return info;
}

bool IsPointOnVoiceBar(POINT pt, const RECT* cachedRect) {
    if (cachedRect && !IsRectEmpty(cachedRect) && PtInRect(cachedRect, pt)) return true;

    VoiceBarInfo live = QueryVoiceBar();
    if (live.visible && PtInRect(&live.rect, pt)) return true;

    HWND h = WindowFromPoint(pt);
    std::vector<DWORD> pids = HostPids();
    if (h) {
        if (IsHostWindow(h, pids)) return true;
        HWND root = GetAncestor(h, GA_ROOT);
        if (IsHostWindow(root, pids)) return true;
        if (root) {
            struct Ctx { const std::vector<DWORD>* pids; bool found = false; } ctx{&pids};
            EnumChildWindows(root, [](HWND ch, LPARAM lp) -> BOOL {
                auto* c = (Ctx*)lp;
                if (IsHostWindow(ch, *c->pids) && IsShown(ch)) { c->found = true; return FALSE; }
                return TRUE;
            }, (LPARAM)&ctx);
            if (ctx.found) return true;
        }
    }
    if (InitVoiceBar() && !pids.empty()) {
        IUIAutomationElement* el = nullptr;
        if (SUCCEEDED(s_uia->ElementFromPoint(pt, &el)) && el) {
            int pid = 0;
            bool match = false;
            if (SUCCEEDED(el->get_CurrentProcessId(&pid))) {
                for (DWORD p : pids) match |= ((DWORD)pid == p);
            }
            el->Release();
            if (match) return true;
        }
    }
    return false;
}

void LogTopWindowsOnce() {
    std::vector<DWORD> pids = HostPids();
    HWND h = nullptr;
    for (int guard = 0; guard < 2000 && (h = FindWindowExW(nullptr, h, nullptr, nullptr)) != nullptr; ++guard) {
        if (!IsShown(h)) continue;
        LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
        bool host = IsHostWindow(h, pids);
        if (!host && !(ex & WS_EX_TOPMOST)) continue;
        wchar_t cls[64] = L"", title[64] = L"";
        GetClassNameW(h, cls, 64);
        GetWindowTextW(h, title, 64);
        RECT r{};
        GetWindowRect(h, &r);
        if (host || (r.right - r.left < 900 && r.bottom - r.top < 500)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            LogSession(L"  TopWin pid=%lu host=%d cls=\"%ls\" title=\"%ls\" rect=(%ld,%ld,%ld,%ld)",
                       pid, host, cls, title, r.left, r.top, r.right, r.bottom);
        }
    }
}

// ---- --probe: log TextInputHost windows + UIA controls whenever the state changes ----
int RunProbe() {
    InitVoiceBar();
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    *wcsrchr(path, L'\\') = 0;
    lstrcatW(path, L"\\probe.log");
    FILE* f = _wfopen(path, L"w, ccs=UTF-8");
    if (!f) { CleanupVoiceBar(); return 1; }
    std::wstring last;
    DWORD start = GetTickCount();
    while (GetTickCount() - start < 60000) {
        std::vector<DWORD> pids = HostPids();
        struct Ctx { const std::vector<DWORD>* pids; std::wstring s; } ctx{&pids, L""};
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            auto* c = (Ctx*)lp;
            wchar_t cls[128] = L"", title[128] = L"", line[512];
            GetClassNameW(h, cls, 128);
            bool host = IsHostWindow(h, *c->pids);
            bool appFrame = !host && !lstrcmpW(cls, L"ApplicationFrameWindow") && IsShown(h);
            if (!host && !appFrame) return TRUE;
            GetWindowTextW(h, title, 128);
            DWORD cloaked = 0;
            DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            RECT r{};
            GetWindowRect(h, &r);
            if (host) {
                swprintf(line, 512, L"  TOP hwnd=%p class=%ls title=\"%ls\" vis=%d clk=%lu rect=(%ld,%ld,%ld,%ld) shown=%d\n",
                         (void*)h, cls, title, IsWindowVisible(h), cloaked, r.left, r.top, r.right, r.bottom, IsShown(h));
                c->s += line;
            }
            EnumChildWindows(h, [](HWND ch, LPARAM lp2) -> BOOL {
                auto* c2 = (Ctx*)lp2;
                if (!IsHostWindow(ch, *c2->pids)) return TRUE;
                wchar_t ccls[128] = L"", cline[512];
                GetClassNameW(ch, ccls, 128);
                RECT cr{};
                GetWindowRect(ch, &cr);
                swprintf(cline, 512, L"    CHILD hwnd=%p class=%ls vis=%d rect=(%ld,%ld,%ld,%ld)\n",
                         (void*)ch, ccls, IsWindowVisible(ch), cr.left, cr.top, cr.right, cr.bottom);
                c2->s += cline;
                return TRUE;
            }, lp);
            return TRUE;
        }, (LPARAM)&ctx);

        VoiceBarInfo bar = QueryVoiceBar();
        if (bar.visible) {
            wchar_t bline[256];
            swprintf(bline, 256, L"  UIA/BAR visible=1 rect=(%ld,%ld,%ld,%ld)\n",
                     bar.rect.left, bar.rect.top, bar.rect.right, bar.rect.bottom);
            ctx.s += bline;
        }
        if (ctx.s != last) {
            fwprintf(f, L"[t=%5lums] barVisible=%d\n%ls\n", GetTickCount() - start, bar.visible, ctx.s.c_str());
            fflush(f);
            last = ctx.s;
        }
        Sleep(200);
    }
    fclose(f);
    CleanupVoiceBar();
    return 0;
}
