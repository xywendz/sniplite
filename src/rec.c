/* Video recording: a Media Foundation H.264 SinkWriter fed with GDI frames.
 * Frames come from the same screen DC a screenshot uses, so a window recording
 * captures what is on screen (including overlapping windows at its edges). */
#include "feature.h"
#include "config.h"
#include "lang.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shellapi.h>

#ifndef MF_VERSION
#define MF_VERSION 0x00020070
#endif

#define REC_TIMER 7
#define REC_DONE_MSG (WM_APP + 11)

typedef struct {
    HINSTANCE inst;
    HWND daemon;
    HWND ind;
    HWND pick;
    HWND pick_list;
    HWND track_hwnd;   /* the window being recorded, NULL for full screen */
    bool active;
    bool stopping;
    HANDLE stop_evt;
    HANDLE done_evt;
    HANDLE thread;
    RECT rect;
    int fps;
    int bitrate;
    WCHAR path[MAX_PATH * 2];
    DWORD started;
    UINT_PTR timer;
    bool result;
    WCHAR error[160];
} Rec;

static Rec R;

bool rec_active(void) { return R.active; }

static int parse_i(const WCHAR *s)
{
    int v = 0;
    bool neg = *s == L'-';
    if (neg)
        s++;
    while (*s >= L'0' && *s <= L'9')
        v = v * 10 + (*s++ - L'0');
    return neg ? -v : v;
}

/* ---- output path ---------------------------------------------------------------- */

static void stamp(WCHAR *s, int cap)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    wsprintfW(s, L"%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

static void default_path(WCHAR *out, int cap)
{
    WCHAR base[MAX_PATH];
    base[0] = 0;
    if (!GetEnvironmentVariableW(L"USERPROFILE", base, COUNTOF(base)) || !base[0])
        GetModuleFileNameW(NULL, base, MAX_PATH);
    WCHAR s[32];
    stamp(s, COUNTOF(s));
    wsprintfW(out, L"%s\\Pictures\\sniplite-%s.mp4", base, s);
    if (GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES) {
        WCHAR dir[MAX_PATH];
        wsprintfW(dir, L"%s\\Pictures", base);
        CreateDirectoryW(dir, NULL);
    }
}

/* ---- indicator ------------------------------------------------------------------- */

static void ind_text(WCHAR *s, int cap)
{
    DWORD sec = (GetTickCount() - R.started) / 1000;
    wsprintfW(s, L"%s %02u:%02u", T(RC_TIME), sec / 60, sec % 60);
}

static LRESULT CALLBACK ind_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        HBRUSH bg = CreateSolidBrush(RGB(32, 34, 38));
        FillRect(dc, &r, bg);
        DeleteObject(bg);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(230, 70, 70));
        HGDIOBJ old = SelectObject(dc, pen);
        MoveToEx(dc, 8, r.bottom - 6, NULL);
        LineTo(dc, 216, r.bottom - 6);
        SelectObject(dc, old);
        DeleteObject(pen);
        WCHAR s[128];
        ind_text(s, COUNTOF(s));
        SetTextColor(dc, RGB(240, 240, 242));
        SetBkMode(dc, TRANSPARENT);
        HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
        if (f)
            SelectObject(dc, f);
        TextOutW(dc, 10, 8, s, lstrlenW(s));
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        rec_stop();
        return 0;
    case WM_TIMER:
        if (wp == REC_TIMER) {
            InvalidateRect(h, NULL, FALSE);
            if (R.done_evt && WaitForSingleObject(R.done_evt, 0) == WAIT_OBJECT_0) {
                KillTimer(h, REC_TIMER);
                R.timer = 0;
                PostMessageW(R.daemon, REC_DONE_MSG, (WPARAM)R.result, 0);
            }
        }
        return 0;
    case WM_DESTROY:
        if (R.timer)
            KillTimer(h, REC_TIMER);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static bool ind_create(void)
{
    static bool registered;
    if (!registered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = ind_proc;
        wc.hInstance = R.inst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = L"sniplite.recind";
        registered = RegisterClassW(&wc) != 0;
    }
    if (!registered)
        return false;
    int x = GetSystemMetrics(SM_CXVIRTUALSCREEN) - 240;
    int y = GetSystemMetrics(SM_CYVIRTUALSCREEN) + GetSystemMetrics(SM_YVIRTUALSCREEN) + 12;
    R.ind = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"sniplite.recind",
                            L"sniplite", WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS, x, y, 226, 34, NULL, NULL, R.inst, NULL);
    if (!R.ind)
        return false;
    SetLayeredWindowAttributes(R.ind, 0, 225, LWA_ALPHA);
    HFONT f = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, T(FONT));
    if (f)
        SendMessageW(R.ind, WM_SETFONT, (WPARAM)f, 0);
    R.timer = SetTimer(R.ind, REC_TIMER, 250, NULL) != 0;
    return true;
}

static void ind_destroy(void)
{
    if (R.ind) {
        DestroyWindow(R.ind);
        R.ind = NULL;
    }
}

/* ---- worker thread ---------------------------------------------------------------- */

static void rec_err(const WCHAR *s)
{
    lstrcpynW(R.error, s, COUNTOF(R.error));
}

static bool write_frame(IMFSinkWriter *sw, DWORD stream, const uint8_t *px, LONG stride, int h, int fps,
                        LONGLONG frame)
{
    IMFMediaBuffer *buf = NULL;
    if (FAILED(MFCreateMemoryBuffer((DWORD)(stride * h), &buf)))
        return false;
    uint8_t *dst = NULL;
    bool ok = false;
    if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &dst, NULL, NULL))) {
        memcpy(dst, px, (size_t)(stride * h));
        IMFMediaBuffer_Unlock(buf);
        IMFMediaBuffer_SetCurrentLength(buf, (DWORD)(stride * h));
        IMFSample *sample = NULL;
        if (SUCCEEDED(MFCreateSample(&sample))) {
            LONGLONG dur = 10000000LL / fps;
            if (SUCCEEDED(IMFSample_AddBuffer(sample, buf)) &&
                SUCCEEDED(IMFSample_SetSampleTime(sample, frame * dur)) &&
                SUCCEEDED(IMFSample_SetSampleDuration(sample, dur)) &&
                SUCCEEDED(IMFSinkWriter_WriteSample(sw, stream, sample)))
                ok = true;
            IMFSample_Release(sample);
        }
    }
    IMFMediaBuffer_Release(buf);
    return ok;
}

static DWORD WINAPI rec_thread(void *unused)
{
    (void)unused;
    R.result = false;
    R.error[0] = 0;
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) {
        rec_err(T(RC_NOMF));
        goto done;
    }

    int w = R.rect.right - R.rect.left, h = R.rect.bottom - R.rect.top;
    w &= ~1;
    h &= ~1; /* H.264 needs even dimensions */
    if (w < 16 || h < 16) {
        rec_err(T(CAP_OUTSIDE));
        goto shutdown;
    }

    IMFAttributes *attrs = NULL;
    if (SUCCEEDED(MFCreateAttributes(&attrs, 2))) {
        IMFAttributes_SetUINT32(attrs, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, 1);
        IMFAttributes_SetUINT32(attrs, &MF_SINK_WRITER_DISABLE_THROTTLING, 1);
    }
    IMFSinkWriter *sw = NULL;
    hr = MFCreateSinkWriterFromURL(R.path, NULL, attrs, &sw);
    if (attrs)
        IMFAttributes_Release(attrs);
    if (FAILED(hr)) {
        rec_err(T(SAVE_FAILED));
        goto shutdown;
    }

    IMFMediaType *out = NULL;
    MFCreateMediaType(&out);
    IMFMediaType_SetGUID(out, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(out, &MF_MT_SUBTYPE, &MFVideoFormat_H264);
    IMFMediaType_SetUINT32(out, &MF_MT_AVG_BITRATE, (UINT32)R.bitrate * 1000000);
    IMFMediaType_SetUINT32(out, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    IMFAttributes_SetUINT64((IMFAttributes *)out, &MF_MT_FRAME_SIZE, ((UINT64)(UINT32)w << 32) | (UINT32)h);
    IMFAttributes_SetUINT64((IMFAttributes *)out, &MF_MT_FRAME_RATE, ((UINT64)(UINT32)R.fps << 32) | 1);
    IMFAttributes_SetUINT64((IMFAttributes *)out, &MF_MT_PIXEL_ASPECT_RATIO, ((UINT64)1 << 32) | 1);
    DWORD stream = 0;
    hr = IMFSinkWriter_AddStream(sw, out, &stream);
    IMFMediaType_Release(out);
    if (FAILED(hr)) {
        rec_err(T(RC_FAIL));
        goto release_sw;
    }

    IMFMediaType *in = NULL;
    MFCreateMediaType(&in);
    IMFMediaType_SetGUID(in, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    IMFMediaType_SetGUID(in, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32);
    IMFMediaType_SetUINT32(in, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    IMFAttributes_SetUINT64((IMFAttributes *)in, &MF_MT_FRAME_SIZE, ((UINT64)(UINT32)w << 32) | (UINT32)h);
    IMFAttributes_SetUINT64((IMFAttributes *)in, &MF_MT_FRAME_RATE, ((UINT64)(UINT32)R.fps << 32) | 1);
    IMFAttributes_SetUINT64((IMFAttributes *)in, &MF_MT_PIXEL_ASPECT_RATIO, ((UINT64)1 << 32) | 1);
    hr = IMFSinkWriter_SetInputMediaType(sw, stream, in, NULL);
    IMFMediaType_Release(in);
    if (FAILED(hr)) {
        rec_err(T(RC_FAIL));
        goto release_sw;
    }
    hr = IMFSinkWriter_BeginWriting(sw);
    if (FAILED(hr)) {
        rec_err(T(RC_FAIL));
        goto release_sw;
    }

    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; /* top-down DIB */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    uint8_t *bits = NULL;
    HBITMAP dib = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    HDC mem = dib ? CreateCompatibleDC(NULL) : NULL;
    HGDIOBJ old = mem ? SelectObject(mem, dib) : NULL;
    HDC screen = GetDC(NULL);
    if (!bits || !mem || !screen) {
        rec_err(T(CAP_FAILED));
        if (screen)
            ReleaseDC(NULL, screen);
        if (mem) {
            SelectObject(mem, old);
            DeleteDC(mem);
        }
        if (dib)
            DeleteObject(dib);
        goto release_sw;
    }

    LONGLONG frame = 0;
    double t0 = now_ms();
    double step = 1000.0 / R.fps;
    bool ok = true;
    RECT frame_rect = R.rect;
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    for (;;) {
        if (WaitForSingleObject(R.stop_evt, 0) == WAIT_OBJECT_0)
            break;
        if (R.track_hwnd) {
            RECT wr;
            if (!IsWindow(R.track_hwnd) || IsIconic(R.track_hwnd) || !GetWindowRect(R.track_hwnd, &wr))
                break; /* the window closed: finish the file */
            frame_rect.left = CLAMPI(wr.left, vx, vx + vw - w);
            frame_rect.top = CLAMPI(wr.top, vy, vy + vh - h);
        }
        BitBlt(mem, 0, 0, w, h, screen, frame_rect.left, frame_rect.top, SRCCOPY | CAPTUREBLT);
        /* MF RGB32 is bottom-up: feed the rows in reverse. */
        int row = w * 4;
        for (int y = 0; y < h / 2; y++) {
            uint8_t *a = bits + (size_t)y * row, *b = bits + (size_t)(h - 1 - y) * row;
            for (int i = 0; i < row; i++) {
                uint8_t t = a[i];
                a[i] = b[i];
                b[i] = t;
            }
        }
        if (!write_frame(sw, stream, bits, row, h, R.fps, frame)) {
            ok = false;
            break;
        }
        frame++;
        if (R.stopping)
            break;
        double wait = t0 + (double)frame * step - now_ms();
        if (wait > 0)
            Sleep((DWORD)wait);
    }
    ReleaseDC(NULL, screen);
    SelectObject(mem, old);
    DeleteDC(mem);
    DeleteObject(dib);

    if (FAILED(IMFSinkWriter_Finalize(sw)))
        ok = false;
    R.result = ok && frame > 0;
    if (!R.result)
        rec_err(T(RC_FAIL));
    goto release_sw;

release_sw:
    IMFSinkWriter_Release(sw);
shutdown:
    MFShutdown();
done:
    if (R.done_evt)
        SetEvent(R.done_evt);
    return 0;
}

/* ---- public API ------------------------------------------------------------------ */

bool rec_start(bool fullscreen, HWND hwnd, const WCHAR *path, int fps, int bitrate, Str *err)
{
    if (R.active) {
        if (err)
            str_add(err, T(RC_NOT_ACTIVE));
        return false;
    }
    if (path && lstrlenW(path) >= 4) {
        const WCHAR *ext = path + lstrlenW(path) - 4;
        if (lstrcmpiW(ext, L".mp4")) {
            if (err)
                str_add(err, T(RC_BADFILE));
            return false;
        }
    }
    RECT r;
    if (fullscreen || !hwnd) {
        int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        SetRect(&r, x, y, x + GetSystemMetrics(SM_CXVIRTUALSCREEN), y + GetSystemMetrics(SM_CYVIRTUALSCREEN));
    } else {
        if (!IsWindow(hwnd) || IsIconic(hwnd) || !GetWindowRect(hwnd, &r)) {
            if (err)
                str_add(err, T(CAP_FAILED));
            return false;
        }
        RECT v;
        int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        SetRect(&v, x, y, x + GetSystemMetrics(SM_CXVIRTUALSCREEN), y + GetSystemMetrics(SM_CYVIRTUALSCREEN));
        r = rect_isect(r, v);
    }
    if (rect_empty(&r)) {
        if (err)
            str_add(err, T(CAP_OUTSIDE));
        return false;
    }
    R.rect = r;
    R.track_hwnd = fullscreen || !hwnd ? NULL : hwnd;
    R.fps = CLAMPI(fps, 5, 60);
    R.bitrate = CLAMPI(bitrate, 1, 20);
    if (path && path[0])
        lstrcpynW(R.path, path, COUNTOF(R.path));
    else
        default_path(R.path, COUNTOF(R.path));
    R.stop_evt = CreateEventW(NULL, TRUE, FALSE, NULL);
    R.done_evt = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!R.stop_evt || !R.done_evt) {
        if (R.stop_evt)
            CloseHandle(R.stop_evt);
        if (R.done_evt)
            CloseHandle(R.done_evt);
        R.stop_evt = R.done_evt = NULL;
        if (err)
            str_add(err, T(NO_MEMORY));
        return false;
    }
    R.active = true;
    R.stopping = false;
    R.started = GetTickCount();
    R.result = false;
    if (err)
        str_addf(err, T(RC_STARTED), R.path);
    ind_create();
    R.thread = CreateThread(NULL, 0, rec_thread, NULL, 0, NULL);
    if (!R.thread) {
        rec_stop();
        return false;
    }
    CloseHandle(R.thread);
    R.thread = NULL;
    return true;
}

void rec_stop(void)
{
    if (!R.active || R.stopping)
        return;
    R.stopping = true;
    if (R.stop_evt)
        SetEvent(R.stop_evt);
}

/* Called from the daemon's message loop when the worker has finalized. */
void rec_indicator_tick(void)
{
    if (!R.active || !R.done_evt)
        return;
    if (WaitForSingleObject(R.done_evt, 0) != WAIT_OBJECT_0)
        return;
    bool ok = R.result;
    WCHAR path[MAX_PATH * 2], err[160];
    lstrcpynW(path, R.path, COUNTOF(path));
    lstrcpynW(err, R.error, COUNTOF(err));
    ind_destroy();
    CloseHandle(R.stop_evt);
    CloseHandle(R.done_evt);
    R.stop_evt = R.done_evt = NULL;
    R.active = false;
    R.stopping = false;
    R.track_hwnd = NULL;
    if (ok) {
        Str s = {0};
        str_addf(&s, T(RC_DONE), path);
        MessageBoxW(NULL, s.p, T(RC_MENU), MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        str_free(&s);
    } else {
        Str s = {0};
        str_addf(&s, T(RC_FAIL), err[0] ? err : T(RC_FAIL));
        MessageBoxW(NULL, s.p, T(RC_MENU), MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        str_free(&s);
    }
}

/* ---- window picker ---------------------------------------------------------------- */

#define PICK_FULL   1
#define PICK_WINDOW 2
#define PICK_LIST   3
#define PICK_CANCEL 4

typedef struct {
    HWND hwnd;
    WCHAR title[256];
} PickItem;

static PickItem *pick_items;
static int pick_count;

static BOOL CALLBACK pick_enum(HWND h, LPARAM unused)
{
    (void)unused;
    if (!IsWindowVisible(h) || IsIconic(h))
        return TRUE;
    WCHAR cls[64], title[256];
    GetClassNameW(h, cls, COUNTOF(cls));
    if (!lstrcmpiW(cls, L"sniplite.daemon") || !lstrcmpiW(cls, L"sniplite.overlay") || !lstrcmpiW(cls, L"sniplite.recind") ||
        !lstrcmpiW(cls, L"sniplite.recpick") || !lstrcmpiW(cls, L"sniplite.studio.text"))
        return TRUE;
    title[0] = 0;
    GetWindowTextW(h, title, COUNTOF(title));
    RECT r;
    GetWindowRect(h, &r);
    if (!title[0] || (r.right - r.left) < 32 || (r.bottom - r.top) < 32)
        return TRUE;
    PickItem *p = mem_grow(pick_items, (size_t)(pick_count + 1) * sizeof *pick_items);
    if (!p)
        return FALSE;
    pick_items = p;
    pick_items[pick_count].hwnd = h;
    lstrcpynW(pick_items[pick_count].title, title, COUNTOF(title));
    pick_count++;
    return TRUE;
}

static LRESULT CALLBACK pick_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_COMMAND:
        if (HIWORD(wp) == LBN_DBLCLK && (HWND)lp == R.pick_list) {
            int sel = (int)SendMessageW(R.pick_list, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < pick_count) {
                SetPropW(h, L"sniplite.recchoice", (HANDLE)(INT_PTR)(2 + sel));
                DestroyWindow(h);
            }
            return 0;
        }
        if (LOWORD(wp) == PICK_FULL) {
            SetPropW(h, L"sniplite.recchoice", (HANDLE)(INT_PTR)1);
            DestroyWindow(h);
            return 0;
        }
        if (LOWORD(wp) == PICK_WINDOW) {
            int sel = (int)SendMessageW(R.pick_list, LB_GETCURSEL, 0, 0);
            if (sel < 0 || sel >= pick_count) {
                MessageBoxW(h, T(RC_PICK_NONE), T(RC_MENU), MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            SetPropW(h, L"sniplite.recchoice", (HANDLE)(INT_PTR)(2 + sel));
            DestroyWindow(h);
            return 0;
        }
        if (LOWORD(wp) == PICK_CANCEL || LOWORD(wp) == IDCANCEL) {
            SetPropW(h, L"sniplite.recchoice", (HANDLE)(INT_PTR)0);
            DestroyWindow(h);
            return 0;
        }
        return 0;
    case WM_CLOSE:
        SetPropW(h, L"sniplite.recchoice", (HANDLE)(INT_PTR)0);
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        R.pick = NULL;
        R.pick_list = NULL;
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

void rec_pick(void)
{
    static bool registered;
    if (!registered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = pick_proc;
        wc.hInstance = R.inst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"sniplite.recpick";
        registered = RegisterClassW(&wc) != 0;
    }
    if (!registered)
        return;
    pick_items = NULL;
    pick_count = 0;
    EnumWindows(pick_enum, 0);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"sniplite.recpick", T(RC_MENU),
                               WS_POPUPWINDOW | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, 0, 0, 460, 360, R.daemon,
                               NULL, R.inst, NULL);
    if (!dlg)
        return;
    R.pick = dlg;
    HFONT font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, T(FONT));
    if (font)
        SendMessageW(dlg, WM_SETFONT, (WPARAM)font, 0);
    HWND hint = CreateWindowExW(0, L"Static", T(RC_PICK_HINT), WS_CHILD | WS_VISIBLE, 16, 14, 428, 28, dlg, NULL,
                                R.inst, NULL);
    R.pick_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"ListBox", NULL,
                                  WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP, 16, 50, 428, 236, dlg,
                                  (HMENU)PICK_LIST, R.inst, NULL);
    HWND full = CreateWindowExW(0, L"Button", T(RC_FULL),
                                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP, 16, 300, 132, 30, dlg,
                                (HMENU)PICK_FULL, R.inst, NULL);
    HWND win = CreateWindowExW(0, L"Button", T(RC_WINDOW), WS_CHILD | WS_VISIBLE | WS_TABSTOP, 160, 300, 132, 30,
                               dlg, (HMENU)PICK_WINDOW, R.inst, NULL);
    HWND cancel = CreateWindowExW(0, L"Button", T(O_CANCEL_BTN), WS_CHILD | WS_VISIBLE | WS_TABSTOP, 312, 300, 132, 30,
                                  dlg, (HMENU)PICK_CANCEL, R.inst, NULL);
    HWND controls[] = {hint, R.pick_list, full, win, cancel};
    for (int i = 0; i < COUNTOF(controls); i++)
        if (controls[i] && font)
            SendMessageW(controls[i], WM_SETFONT, (WPARAM)font, 0);
    for (int i = 0; i < pick_count; i++)
        SendMessageW(R.pick_list, LB_ADDSTRING, 0, (LPARAM)pick_items[i].title);
    if (pick_count)
        SendMessageW(R.pick_list, LB_SETCURSEL, 0, 0);
    RECT dr;
    GetWindowRect(dlg, &dr);
    int w = dr.right - dr.left, h = dr.bottom - dr.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    SetWindowPos(dlg, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
    SetForegroundWindow(dlg);
    MSG msg;
    int result = 0;
    while (IsWindow(dlg) && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            result = 0;
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (IsWindow(dlg)) {
        result = (int)(INT_PTR)GetPropW(dlg, L"sniplite.recchoice");
        DestroyWindow(dlg);
    }
    R.pick = R.pick_list = NULL;
    if (font)
        DeleteObject(font);
    Config cfg;
    config_load(&cfg);
    if (result == 1)
        rec_start(true, NULL, NULL, cfg.rec_fps, cfg.rec_bitrate, NULL);
    else if (result >= 2 && result - 2 < pick_count)
        rec_start(false, pick_items[result - 2].hwnd, NULL, cfg.rec_fps, cfg.rec_bitrate, NULL);
    if (pick_items) {
        mem_free(pick_items);
        pick_items = NULL;
    }
    pick_count = 0;
}

/* ---- CLI and lifecycle ------------------------------------------------------------ */

int rec_cmd(int argc, WCHAR **argv, const WCHAR *cwd, bool json, Str *out)
{
    (void)cwd;
    (void)json;
    for (int i = 1; i < argc; i++) {
        if (arg_eq(argv[i], L"--stop")) {
            if (!rec_active())
                str_add(out, T(RC_NOT_ACTIVE));
            else
                rec_stop();
            return EXIT_OK;
        }
    }
    Config cfg;
    config_load(&cfg);
    bool full = false, win = false;
    const WCHAR *file = NULL;
    int fps = cfg.rec_fps, bitrate = cfg.rec_bitrate;
    for (int i = 1; i < argc; i++) {
        const WCHAR *a = argv[i];
        if (arg_eq(a, L"--json"))
            continue;
        if (arg_eq(a, L"--full")) {
            full = true;
            continue;
        }
        if (arg_eq(a, L"--window")) {
            win = true;
            continue;
        }
        if (arg_eq(a, L"--out") || arg_eq(a, L"--fps") || arg_eq(a, L"--bitrate")) {
            if (i + 1 >= argc) {
                str_add(out, T(RC_USAGE));
                return EXIT_USAGE;
            }
            const WCHAR *v = argv[++i];
            if (arg_eq(a, L"--out"))
                file = v;
            else if (arg_eq(a, L"--fps"))
                fps = parse_i(v);
            else
                bitrate = parse_i(v);
            continue;
        }
        str_add(out, T(RC_USAGE));
        return EXIT_USAGE;
    }
    if (win || (!full && !file)) {
        rec_pick();
        return EXIT_OK;
    }
    Str err = {0};
    bool ok = rec_start(full, NULL, file, fps, bitrate, &err);
    if (!ok && err.n)
        str_add(out, err.p);
    str_free(&err);
    return ok ? EXIT_OK : EXIT_FAIL;
}

bool rec_init(HINSTANCE inst, HWND daemon)
{
    R.inst = inst;
    R.daemon = daemon;
    return true;
}

void rec_shutdown(void)
{
    if (R.active)
        rec_stop();
    ind_destroy();
    if (R.stop_evt)
        CloseHandle(R.stop_evt);
    if (R.done_evt)
        CloseHandle(R.done_evt);
    R.stop_evt = R.done_evt = NULL;
    R.track_hwnd = NULL;
    if (pick_items) {
        mem_free(pick_items);
        pick_items = NULL;
    }
}
