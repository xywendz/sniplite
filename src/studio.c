/* The creative studio: [Create Puzzle] arranges images freely or into a split
 * layout, adjusts layer order and adds simple annotations; [Merge Document]
 * reorders images and exports them to PDF, PPTX, PNG, WEBP or JPG. */
#include "feature.h"
#include "config.h"
#include "image.h"
#include "lang.h"
#include <commdlg.h>
#include <windowsx.h>

#define ST_TOOLBAR 76
#define CANVAS_MARGIN 16

typedef enum { LAY_FREE = 0, LAY_H2, LAY_V2, LAY_GRID4, LAY_GRID6, LAY_COUNT } Layout;
typedef enum { TOOL_SELECT = 0, TOOL_RECT, TOOL_ARROW, TOOL_TEXT, TOOL_MOSAIC, TOOL_COUNT } Tool;
typedef enum { ANN_RECT = 0, ANN_ARROW, ANN_TEXT, ANN_MOSAIC } AnnType;

typedef struct StImage {
    Img img;
    int x, y, w, h;
    struct StImage *next;
} StImage;

typedef struct Ann {
    AnnType type;
    POINT a, b;
    WCHAR text[128];
    struct Ann *next;
} Ann;

static struct {
    HINSTANCE inst;
    HWND daemon;
    HWND wnd;
    bool open;
    int mode;
    StImage *head, *tail, *sel;
    int count;
    Ann *anns, *ann_tail;
    int ann_count;
    Layout layout;
    Tool tool;
    int fmt, quality;
    bool drawing;
    POINT drag_start, drag_orig;
    bool moving;
    HFONT font;
    Img back;
} T;

static const WCHAR *kLayoutText[LAY_COUNT] = {NULL, NULL, NULL, NULL, NULL};
static const WCHAR *kToolText[TOOL_COUNT] = {NULL, NULL, NULL, NULL, NULL};
static const WCHAR *kFmtText[5] = {NULL, NULL, NULL, NULL, NULL};

static void refresh_labels(void);

static void init_texts(void)
{
    kLayoutText[LAY_FREE] = T(STU_FREE);
    kLayoutText[LAY_H2] = T(STU_H2);
    kLayoutText[LAY_V2] = T(STU_V2);
    kLayoutText[LAY_GRID4] = T(STU_GRID4);
    kLayoutText[LAY_GRID6] = T(STU_GRID6);
    kToolText[TOOL_SELECT] = T(STU_SELECT);
    kToolText[TOOL_RECT] = T(STU_RECT);
    kToolText[TOOL_ARROW] = T(STU_ARROW);
    kToolText[TOOL_TEXT] = T(STU_TEXT);
    kToolText[TOOL_MOSAIC] = T(STU_MOSAIC);
    kFmtText[0] = T(STU_PDF);
    kFmtText[1] = T(STU_PPT);
    kFmtText[2] = T(STU_PNG);
    kFmtText[3] = T(STU_WEBP);
    kFmtText[4] = T(STU_JPG);
}

/* ---- list operations -------------------------------------------------------------- */

static void relayout(void)
{
    if (T.layout == LAY_FREE || !T.count)
        return;
    int cols = T.layout == LAY_H2 || T.layout == LAY_GRID4 || T.layout == LAY_GRID6 ? 2 : 1;
    int rows = T.layout == LAY_V2 ? 2 : T.layout == LAY_GRID4 ? 2 : T.layout == LAY_GRID6 ? 3 : 1;
    RECT r;
    GetClientRect(T.wnd, &r);
    int cw = (r.right - 2 * CANVAS_MARGIN) / cols;
    int ch = (r.bottom - ST_TOOLBAR - 2 * CANVAS_MARGIN) / rows;
    int i = 0;
    for (StImage *im = T.head; im && i < cols * rows; im = im->next, i++) {
        float s = MINI((cw - 12) / (float)im->img.w, (ch - 12) / (float)im->img.h);
        im->w = MAXI(1, (int)(im->img.w * s));
        im->h = MAXI(1, (int)(im->img.h * s));
        im->x = CANVAS_MARGIN + (i % cols) * cw + (cw - im->w) / 2;
        im->y = ST_TOOLBAR + CANVAS_MARGIN + (i / cols) * ch + (ch - im->h) / 2;
    }
}

static StImage *image_add_file(const WCHAR *path)
{
    Img im;
    if (!image_load(path, &im))
        return NULL;
    StImage *si = mem_alloc(sizeof *si);
    if (!si) {
        img_destroy(&im);
        return NULL;
    }
    si->img = im;
    RECT r;
    GetClientRect(T.wnd, &r);
    int x = CANVAS_MARGIN + (T.count % 6) * 24, y = ST_TOOLBAR + CANVAS_MARGIN + (T.count % 6) * 24;
    int maxw = MAXI(64, (r.right - r.left) / 2), maxh = MAXI(64, (r.bottom - r.top - ST_TOOLBAR) / 2);
    float s = MINI(maxw / (float)im.w, maxh / (float)im.h);
    if (s > 1)
        s = 1;
    si->w = MAXI(1, (int)(im.w * s));
    si->h = MAXI(1, (int)(im.h * s));
    si->x = x;
    si->y = y;
    if (T.tail)
        T.tail->next = si;
    else
        T.head = si;
    T.tail = si;
    T.sel = si;
    T.count++;
    relayout();
    return si;
}

/* Rebuild the list from an array: small counts and clearer than pointer surgery. */
static void rebuild_order(StImage **arr, int n)
{
    T.head = T.tail = NULL;
    for (int i = 0; i < n; i++) {
        arr[i]->next = NULL;
        if (T.tail)
            T.tail->next = arr[i];
        else
            T.head = arr[i];
        T.tail = arr[i];
    }
}

static void move_sel(int delta)
{
    if (!T.sel || T.count < 2)
        return;
    StImage *arr[1024];
    int n = 0;
    for (StImage *im = T.head; im && n < 1024; im = im->next)
        arr[n++] = im;
    int at = 0;
    while (at < n && arr[at] != T.sel)
        at++;
    int to = CLAMPI(at + delta, 0, n - 1);
    if (to == at)
        return;
    StImage *s = arr[at];
    if (to < at)
        for (int i = at; i > to; i--)
            arr[i] = arr[i - 1];
    else
        for (int i = at; i < to; i++)
            arr[i] = arr[i + 1];
    arr[to] = s;
    rebuild_order(arr, n);
}

static void layer_sel(bool front)
{
    move_sel(front ? T.count : -T.count); /* the tail is drawn last, i.e. on top */
}

static void delete_sel(void)
{
    if (!T.sel)
        return;
    StImage **p = &T.head;
    while (*p && *p != T.sel)
        p = &(*p)->next;
    if (!*p)
        return;
    StImage *it = T.sel;
    *p = it->next;
    if (T.tail == it) {
        T.tail = T.head;
        while (T.tail && T.tail->next)
            T.tail = T.tail->next;
    }
    T.sel = *p ? *p : T.head;
    img_destroy(&it->img);
    mem_free(it);
    T.count--;
}

static void clear_all(void)
{
    while (T.head) {
        StImage *n = T.head->next;
        img_destroy(&T.head->img);
        mem_free(T.head);
        T.head = n;
    }
    T.head = T.tail = T.sel = NULL;
    T.count = 0;
    Ann *a = T.anns;
    while (a) {
        Ann *n = a->next;
        mem_free(a);
        a = n;
    }
    T.anns = T.ann_tail = NULL;
    T.ann_count = 0;
}

static StImage *hit_test(POINT pt)
{
    StImage *found = NULL;
    for (StImage *im = T.head; im; im = im->next)
        if (pt.x >= im->x && pt.x < im->x + im->w && pt.y >= im->y && pt.y < im->y + im->h)
            found = im; /* last match: topmost */
    return found;
}

/* ---- text input -------------------------------------------------------------------- */

typedef struct {
    HWND edit;
    WCHAR *out;
    int cap;
} TextDlg;

static LRESULT CALLBACK text_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            TextDlg *d = (TextDlg *)GetWindowLongPtrW(h, GWLP_USERDATA);
            GetWindowTextW(d->edit, d->out, d->cap);
            DestroyWindow(h);
        } else if (LOWORD(wp) == IDCANCEL)
            DestroyWindow(h);
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static bool ask_text(HWND parent, WCHAR *buf, int cap)
{
    static bool reg;
    if (!reg) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = text_proc;
        wc.hInstance = T.inst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"sniplite.studio.text";
        reg = RegisterClassW(&wc) != 0;
    }
    if (!reg)
        return false;
    TextDlg *d = mem_alloc(sizeof *d);
    if (!d)
        return false;
    d->out = buf;
    d->cap = cap;
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME, L"sniplite.studio.text", T(STU_TEXT),
                             WS_POPUPWINDOW | WS_CAPTION | WS_VISIBLE | WS_SYSMENU, 0, 0, 340, 150, parent, NULL,
                             T.inst, NULL);
    if (!h) {
        mem_free(d);
        return false;
    }
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)d);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"Edit", buf, WS_CHILD | WS_VISIBLE | ES_MULTILINE, 12, 12, 316, 74, h,
                    (HMENU)101, T.inst, NULL);
    d->edit = GetDlgItem(h, 101);
    CreateWindowExW(0, L"Button", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 76, 100, 88, 28, h, (HMENU)IDOK,
                    T.inst, NULL);
    CreateWindowExW(0, L"Button", T(O_CANCEL_BTN), WS_CHILD | WS_VISIBLE, 178, 100, 88, 28, h, (HMENU)IDCANCEL,
                    T.inst, NULL);
    RECT r;
    GetWindowRect(h, &r);
    SetWindowPos(h, HWND_TOPMOST, (GetSystemMetrics(SM_CXSCREEN) - (r.right - r.left)) / 2,
                 (GetSystemMetrics(SM_CYSCREEN) - (r.bottom - r.top)) / 2, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(h);
    MSG msg;
    while (IsWindow(h) && GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    mem_free(d);
    return buf[0] != 0;
}

/* ---- painting ---------------------------------------------------------------------- */

static void fill_back(void)
{
    RECT r;
    GetClientRect(T.wnd, &r);
    int w = r.right - r.left, h = r.bottom - r.top - ST_TOOLBAR;
    if (w <= 0 || h <= 0)
        return;
    if (!T.back.bmp || T.back.w != w || T.back.h != h) {
        if (T.back.bmp)
            img_destroy(&T.back);
        if (!img_create(&T.back, w, h))
            return;
    }
    HBRUSH bg = CreateSolidBrush(RGB(246, 248, 251));
    FillRect(T.back.dc, &(RECT){0, 0, w, h}, bg);
    DeleteObject(bg);

    if (T.mode == STUDIO_MERGE) {
        int y = CANVAS_MARGIN;
        int i = 1;
        for (StImage *im = T.head; im && y < h - 20; im = im->next, i++) {
            int maxw = w - 2 * CANVAS_MARGIN - 40;
            float s = MINI(maxw / (float)im->img.w, 100.0f / (float)im->img.h);
            int iw = MAXI(1, (int)(im->img.w * s)), ih = MAXI(1, (int)(im->img.h * s));
            StretchBlt(T.back.dc, CANVAS_MARGIN + 30, y, iw, ih, im->img.dc, 0, 0, im->img.w, im->img.h, SRCCOPY);
            HPEN p = CreatePen(PS_SOLID, im == T.sel ? 2 : 1, im == T.sel ? RGB(26, 140, 255) : RGB(190, 198, 210));
            HGDIOBJ old = SelectObject(T.back.dc, p);
            Rectangle(T.back.dc, CANVAS_MARGIN + 29, y - 1, CANVAS_MARGIN + 31 + iw, y + ih + 1);
            SelectObject(T.back.dc, old);
            DeleteObject(p);
            WCHAR n[16];
            wsprintfW(n, L"%d", i);
            SetTextColor(T.back.dc, RGB(60, 66, 76));
            SetBkMode(T.back.dc, TRANSPARENT);
            TextOutW(T.back.dc, CANVAS_MARGIN, y + 4, n, lstrlenW(n));
            y += ih + 12;
        }
        if (!T.head) {
            RECT tr = {0, 0, w, h};
            DrawTextW(T.back.dc, T(STU_MERGE_HINT), -1, &tr, DT_CENTER | DT_VCENTER);
        }
    } else {
        for (StImage *im = T.head; im; im = im->next) {
            StretchBlt(T.back.dc, im->x, im->y - ST_TOOLBAR, im->w, im->h, im->img.dc, 0, 0, im->img.w, im->img.h,
                       SRCCOPY);
            if (im == T.sel) {
                HPEN p = CreatePen(PS_SOLID, 2, RGB(26, 140, 255));
                HGDIOBJ old = SelectObject(T.back.dc, p);
                Rectangle(T.back.dc, im->x - 1, im->y - ST_TOOLBAR - 1, im->x + im->w + 1,
                          im->y - ST_TOOLBAR + im->h + 1);
                SelectObject(T.back.dc, old);
                DeleteObject(p);
            }
        }
        for (Ann *a = T.anns; a; a = a->next) {
            int ax = a->a.x, ay = a->a.y - ST_TOOLBAR, bx = a->b.x, by = a->b.y - ST_TOOLBAR;
            HPEN p = CreatePen(PS_SOLID, 2, RGB(230, 60, 60));
            HGDIOBJ old = SelectObject(T.back.dc, p);
            SetBkMode(T.back.dc, TRANSPARENT);
            if (a->type == ANN_RECT) {
                HGDIOBJ br = SelectObject(T.back.dc, GetStockObject(NULL_BRUSH));
                Rectangle(T.back.dc, ax, ay, bx, by);
                SelectObject(T.back.dc, br);
            } else if (a->type == ANN_ARROW) {
                MoveToEx(T.back.dc, ax, ay, NULL);
                LineTo(T.back.dc, bx, by);
                int dx = bx - ax, dy = by - ay;
                int len = 1;
                for (int i = 0; i < 2; i++)
                    if (dx > len || -dx > len || dy > len || -dy > len)
                        len *= 2;
                if (len > 64)
                    len = 64;
                if (dx != 0 || dy != 0) {
                    int ux = dx * 8 / len, uy = dy * 8 / len; /* unit vector times 8 */
                    MoveToEx(T.back.dc, bx - ux + uy / 2, by - uy - ux / 2, NULL);
                    LineTo(T.back.dc, bx, by);
                    MoveToEx(T.back.dc, bx - ux - uy / 2, by - uy + ux / 2, NULL);
                    LineTo(T.back.dc, bx, by);
                }
            } else if (a->type == ANN_TEXT) {
                SetTextColor(T.back.dc, RGB(230, 60, 60));
                if (T.font)
                    SelectObject(T.back.dc, T.font);
                TextOutW(T.back.dc, ax, ay, a->text, lstrlenW(a->text));
            } else if (a->type == ANN_MOSAIC) {
                int x0 = MINI(ax, bx), x1 = MAXI(ax, bx), y0 = MINI(ay, by), y1 = MAXI(ay, by);
                for (int y = y0; y < y1; y += 8)
                    for (int x = x0; x < x1; x += 8) {
                        COLORREF c = GetPixel(T.back.dc, x, y);
                        HBRUSH br = CreateSolidBrush(c);
                        RECT brr = {x, y, MINI(x + 8, x1), MINI(y + 8, y1)};
                        FillRect(T.back.dc, &brr, br);
                        DeleteObject(br);
                    }
            }
            SelectObject(T.back.dc, old);
            DeleteObject(p);
        }
        if (!T.head && !T.anns) {
            RECT tr = {0, 0, w, h};
            DrawTextW(T.back.dc, T(STU_PUZZLE_HINT), -1, &tr, DT_CENTER | DT_VCENTER);
        }
    }
}

static LRESULT CALLBACK studio_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_SIZE:
        relayout();
        InvalidateRect(h, NULL, TRUE);
        return 0;
    case WM_PAINT: {
        fill_back();
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        HBRUSH tb = CreateSolidBrush(RGB(232, 236, 242));
        FillRect(dc, &(RECT){0, 0, r.right, ST_TOOLBAR}, tb);
        DeleteObject(tb);
        HPEN line = CreatePen(PS_SOLID, 1, RGB(190, 198, 210));
        HGDIOBJ old = SelectObject(dc, line);
        MoveToEx(dc, 0, ST_TOOLBAR - 1, NULL);
        LineTo(dc, r.right, ST_TOOLBAR - 1);
        static const int kSeps[] = {218, 292, 482};
        for (int i = 0; i < COUNTOF(kSeps); i++) {
            MoveToEx(dc, kSeps[i], 8, NULL);
            LineTo(dc, kSeps[i], i == 1 ? 66 : 34);
        }
        SelectObject(dc, old);
        DeleteObject(line);
        if (T.back.bmp)
            BitBlt(dc, 0, ST_TOOLBAR, T.back.w, T.back.h, T.back.dc, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == 1) { /* add images */
            WCHAR file[8192];
            memset(file, 0, sizeof file);
            OPENFILENAMEW of = {0};
            of.lStructSize = sizeof of;
            of.hwndOwner = h;
            of.lpstrFilter = lang_save_filter();
            of.lpstrFile = file;
            of.nMaxFile = COUNTOF(file);
            of.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST;
            if (GetOpenFileNameW(&of)) {
                if (file[of.nFileOffset - 1]) {
                    image_add_file(file);
                } else {
                    WCHAR dir[MAX_PATH];
                    lstrcpynW(dir, file, COUNTOF(dir));
                    for (WCHAR *p = file + of.nFileOffset; *p;) {
                        WCHAR path[MAX_PATH * 2];
                        wsprintfW(path, L"%s\\%s", dir, p);
                        image_add_file(path);
                        p += lstrlenW(p) + 1;
                    }
                }
            }
        } else if (id == 2)
            delete_sel();
        else if (id == 3)
            move_sel(-1);
        else if (id == 4)
            move_sel(1);
        else if (id == 5)
            layer_sel(true);
        else if (id == 6)
            layer_sel(false);
        else if (id == 7)
            clear_all();
        else if (id == 8)
            T.layout = (Layout)((T.layout + 1) % LAY_COUNT), relayout();
        else if (id == 9)
            T.tool = (Tool)((T.tool + 1) % TOOL_COUNT);
        else if (id == 10)
            T.fmt = (T.fmt + 1) % 5;
        else if (id == 11) {
            Config c;
            config_load(&c);
            c.export_quality = c.export_quality >= 100 ? 10 : c.export_quality + 5;
            config_save(&c);
            T.quality = c.export_quality;
        } else if (id == 12) { /* export */
            if (!T.count && !T.anns) {
                MessageBoxW(h, T(STU_NO_IMAGES), T(STU_TITLE), MB_OK | MB_ICONINFORMATION);
                break;
            }
            WCHAR path[MAX_PATH * 2];
            memset(path, 0, sizeof path);
            static const WCHAR *exts[] = {L"pdf", L"pptx", L"png", L"webp", L"jpg"};
            wsprintfW(path, L"sniplite.%s", exts[T.fmt]);
            OPENFILENAMEW of = {0};
            of.lStructSize = sizeof of;
            of.hwndOwner = h;
            of.lpstrFile = path;
            of.nMaxFile = COUNTOF(path);
            of.lpstrFilter = lang_save_filter();
            of.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
            if (GetSaveFileNameW(&of)) {
                ExportPage *pages = NULL;
                int count = 0;
                if (T.mode == STUDIO_MERGE) {
                    count = T.count;
                    if (count)
                        pages = mem_alloc((size_t)count * sizeof *pages);
                    int pi = 0;
                    for (StImage *im = T.head; im && pi < count; im = im->next, pi++) {
                        pages[pi].px = im->img.px;
                        pages[pi].w = im->img.w;
                        pages[pi].h = im->img.h;
                    }
                } else {
                    /* The puzzle is one composed page: images, layout and annotations. */
                    fill_back();
                    if (T.back.bmp) {
                        pages = mem_alloc(sizeof *pages);
                        if (pages) {
                            pages->px = T.back.px;
                            pages->w = T.back.w;
                            pages->h = T.back.h;
                            count = 1;
                        }
                    }
                }
                Str err = {0};
                bool ok = pages && export_images(path, T.fmt, T.quality, pages, count, &err);
                if (pages)
                    mem_free(pages);
                Str msg = {0};
                str_add(&msg, ok ? T(STU_DONE) : T(STU_FAIL));
                str_addf(&msg, ok ? L"%s\n" : L"%s\n", ok ? path : (err.n ? err.p : L""));
                MessageBoxW(h, msg.p, T(STU_TITLE), ok ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONWARNING);
                str_free(&msg);
                str_free(&err);
            }
        } else if (id == 13) { /* mode: puzzle */
            T.mode = STUDIO_PUZZLE;
            SetWindowTextW(h, T(STU_PUZZLE));
            InvalidateRect(h, NULL, TRUE);
        } else if (id == 14) { /* mode: merge */
            T.mode = STUDIO_MERGE;
            SetWindowTextW(h, T(STU_MERGE));
            InvalidateRect(h, NULL, TRUE);
        }
        refresh_labels();
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (pt.y < ST_TOOLBAR)
            return 0;
        if (T.mode == STUDIO_PUZZLE && T.tool == TOOL_TEXT) {
            WCHAR text[128] = {0};
            if (ask_text(h, text, COUNTOF(text))) {
                Ann *a = mem_alloc(sizeof *a);
                if (a) {
                    a->type = ANN_TEXT;
                    a->a = a->b = pt;
                    lstrcpynW(a->text, text, COUNTOF(a->text));
                    a->next = NULL;
                    if (T.ann_tail)
                        T.ann_tail->next = a;
                    else
                        T.anns = a;
                    T.ann_tail = a;
                    T.ann_count++;
                }
            }
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        if (T.mode == STUDIO_PUZZLE && T.tool != TOOL_SELECT) {
            T.drawing = true;
            T.drag_start = pt;
            return 0;
        }
        StImage *hit = hit_test(pt);
        T.sel = hit;
        if (hit && T.mode == STUDIO_PUZZLE) {
            T.moving = true;
            T.drag_start = pt;
            T.drag_orig.x = hit->x;
            T.drag_orig.y = hit->y;
            SetCapture(h);
        }
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (T.moving && T.sel) {
            T.sel->x = T.drag_orig.x + pt.x - T.drag_start.x;
            T.sel->y = T.drag_orig.y + pt.y - T.drag_start.y;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (T.moving) {
            T.moving = false;
            ReleaseCapture();
        }
        if (T.drawing) {
            T.drawing = false;
            Ann *a = mem_alloc(sizeof *a);
            if (a) {
                a->a = T.drag_start;
                a->b = pt;
                a->text[0] = 0;
                if (T.tool == TOOL_RECT)
                    a->type = ANN_RECT;
                else if (T.tool == TOOL_ARROW)
                    a->type = ANN_ARROW;
                else if (T.tool == TOOL_MOSAIC)
                    a->type = ANN_MOSAIC;
                else {
                    mem_free(a);
                    a = NULL;
                }
                if (a) {
                    if (T.ann_tail)
                        T.ann_tail->next = a;
                    else
                        T.anns = a;
                    T.ann_tail = a;
                    T.ann_count++;
                }
            }
        }
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_DELETE)
            delete_sel();
        else if (wp == VK_ESCAPE)
            ShowWindow(h, SW_HIDE);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);
        return 0;
    case WM_DESTROY:
        T.open = false;
        T.wnd = NULL;
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

/* ---- toolbar ----------------------------------------------------------------------- */

static HWND button(HWND parent, const WCHAR *text, int id, int x, int y, int w)
{
    return CreateWindowExW(0, L"Button", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, x, y, w, 26,
                           parent, (HMENU)(INT_PTR)id, T.inst, NULL);
}

static HWND mode_button(HWND parent, const WCHAR *text, int id, int x, int y, int w, bool first)
{
    DWORD style = WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_TABSTOP | (first ? WS_GROUP : 0);
    return CreateWindowExW(0, L"Button", text, style, x, y, w, 26, parent, (HMENU)(INT_PTR)id, T.inst, NULL);
}

static void make_toolbar(HWND h)
{
    /* Row 1: which mode, then the actions common to both. */
    mode_button(h, T(STU_PUZZLE), 13, 10, 8, 96, true);
    mode_button(h, T(STU_MERGE), 14, 112, 8, 96, false);
    button(h, T(STU_ADD), 1, 230, 8, 78);
    button(h, T(STU_DELETE), 2, 316, 8, 64);
    button(h, T(STU_CLEAR), 7, 388, 8, 64);
    /* Row 2: layer order, puzzle tools, then export settings. */
    button(h, T(STU_UP), 3, 10, 40, 54);
    button(h, T(STU_DOWN), 4, 72, 40, 54);
    button(h, T(STU_FRONT), 5, 134, 40, 70);
    button(h, T(STU_BACK), 6, 212, 40, 70);
    button(h, T(STU_LAYOUT), 8, 304, 40, 88);
    button(h, T(STU_TOOL), 9, 400, 40, 72);
    button(h, T(STU_FORMAT), 10, 494, 40, 92);
    button(h, T(STU_QUALITY), 11, 594, 40, 88);
    button(h, T(STU_EXPORT), 12, 690, 40, 80);
}

static void refresh_labels(void)
{
    init_texts();
    if (!T.wnd)
        return;
    CheckDlgButton(T.wnd, 13, T.mode == STUDIO_PUZZLE ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(T.wnd, 14, T.mode == STUDIO_MERGE ? BST_CHECKED : BST_UNCHECKED);
    bool puzzle = T.mode == STUDIO_PUZZLE;
    EnableWindow(GetDlgItem(T.wnd, 5), puzzle);
    EnableWindow(GetDlgItem(T.wnd, 6), puzzle);
    EnableWindow(GetDlgItem(T.wnd, 8), puzzle);
    EnableWindow(GetDlgItem(T.wnd, 9), puzzle);
    SetDlgItemTextW(T.wnd, 8, kLayoutText[T.layout]);
    SetDlgItemTextW(T.wnd, 9, kToolText[T.tool]);
    SetDlgItemTextW(T.wnd, 10, kFmtText[T.fmt]);
    WCHAR q[32];
    wsprintfW(q, L"%s %d", T(STU_QUALITY), T.quality);
    SetDlgItemTextW(T.wnd, 11, q);
}

/* ---- public API -------------------------------------------------------------------- */

void studio_open(int mode)
{
    if (!T.wnd) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = studio_proc;
        wc.hInstance = T.inst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"sniplite.studio";
        if (!RegisterClassW(&wc))
            return;
        Config c;
        config_load(&c);
        T.fmt = CLAMPI(c.export_fmt, 0, 4);
        T.quality = CLAMPI(c.export_quality, 10, 100);
        T.wnd = CreateWindowExW(WS_EX_APPWINDOW, L"sniplite.studio", T(STU_TITLE),
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, 60, 60, 960, 660, NULL, NULL, T.inst, NULL);
        if (!T.wnd)
            return;
        make_toolbar(T.wnd);
        T.font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, T(FONT));
        if (T.font)
            for (int id = 1; id <= 14; id++) {
                HWND ctl = GetDlgItem(T.wnd, id);
                if (ctl)
                    SendMessageW(ctl, WM_SETFONT, (WPARAM)T.font, 0);
            }
        T.open = true;
    }
    T.mode = mode == STUDIO_MERGE ? STUDIO_MERGE : STUDIO_PUZZLE;
    SetWindowTextW(T.wnd, T.mode == STUDIO_MERGE ? T(STU_MERGE) : T(STU_PUZZLE));
    ShowWindow(T.wnd, SW_SHOW);
    SetForegroundWindow(T.wnd);
    refresh_labels();
    InvalidateRect(T.wnd, NULL, TRUE);
}

int studio_cmd(int argc, WCHAR **argv, const WCHAR *cwd, bool json, Str *out)
{
    (void)json;
    int mode = STUDIO_PUZZLE;
    if (argc > 1 && arg_eq(argv[1], L"merge"))
        mode = STUDIO_MERGE;
    studio_open(mode);
    for (int i = 2; i < argc; i++) {
        WCHAR path[MAX_PATH * 2];
        if (cwd && argv[i][0] != L'\\' && argv[i][1] != L':')
            wsprintfW(path, L"%s\\%s", cwd, argv[i]);
        else
            lstrcpynW(path, argv[i], COUNTOF(path));
        if (!image_add_file(path)) {
            str_addf(out, T(STU_FAIL), path);
            return EXIT_FAIL;
        }
    }
    return EXIT_OK;
}

bool studio_init(HINSTANCE inst, HWND daemon)
{
    (void)daemon;
    T.inst = inst;
    init_texts();
    return true;
}

void studio_shutdown(void)
{
    clear_all();
    if (T.back.bmp)
        img_destroy(&T.back);
    if (T.font)
        DeleteObject(T.font);
    if (T.wnd)
        DestroyWindow(T.wnd);
}
