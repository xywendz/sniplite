/* The shelf: a thin bar at the top of the screen that catches screenshots,
 * images, text (including SVG), screen recordings and files. Hover to expand,
 * click to copy, drag out to share, drag in to collect. */
#include "feature.h"
#include "config.h"
#include "image.h"
#include "lang.h"
#include <objidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#define SH_HOVER_TIMER 11
#define SH_COLLAPSE_MS 450
#define THUMB_W 96
#define THUMB_H 64
#define BAR_H 42
#define OPEN_H 140

typedef enum { SH_IMAGE = 0, SH_TEXT, SH_SVG, SH_FILE, SH_VIDEO } ShType;

typedef struct ShelfItem {
    ShType type;
    Img img;   /* full image for image items */
    Img thumb; /* small preview, always present */
    WCHAR *data;
    int len;
    struct ShelfItem *next;
} ShelfItem;

static struct {
    HINSTANCE inst;
    HWND daemon;
    HWND bar;
    ShelfItem *head, *tail;
    int count;
    bool visible;
    bool expanded;
    bool pinned;
    bool tracking;
    int hover_index;
    int thumb_count;
    DWORD leave_at;
    int theme;
    bool always;
    UINT fmt_svg;
    HFONT font;
    bool drag_active;
    ShelfItem *drag_item;
    int down_index;
    POINT down_pt;
    ShelfItem *down_item;
    bool down_active;
} S;

/* ---- themes ---------------------------------------------------------------------- */

typedef struct {
    COLORREF bar, edge, text, accent, hot, thumb_back;
    BYTE alpha;
} Theme;

static const Theme kThemes[SH_THEME_COUNT] = {
    {RGB(246, 248, 251), RGB(206, 214, 224), RGB(36, 42, 52), RGB(26, 140, 255), RGB(220, 238, 255), RGB(232, 238, 246), 242},
    {RGB(30, 32, 38),    RGB(66, 72, 84),    RGB(232, 236, 242), RGB(64, 158, 255), RGB(48, 62, 84),   RGB(44, 48, 56),   235},
    {RGB(70, 82, 104),   RGB(150, 170, 200), RGB(255, 255, 255), RGB(120, 200, 255), RGB(96, 118, 150),RGB(120, 138, 166),190},
    {RGB(238, 240, 244), RGB(198, 204, 214), RGB(48, 52, 60),   RGB(26, 140, 255), RGB(216, 232, 250),RGB(226, 230, 238), 250},
};

static const Theme *theme(void)
{
    int i = CLAMPI(S.theme, 0, SH_THEME_COUNT - 1);
    return &kThemes[i];
}

static void redraw(void)
{
    if (S.bar)
        InvalidateRect(S.bar, NULL, FALSE);
}

/* ---- thumbnails ------------------------------------------------------------------- */

static void make_thumb(ShelfItem *it)
{
    int w = THUMB_W, h = THUMB_H;
    if (it->img.w > 0 && it->img.h > 0) {
        float scale = (float)MINI(w / (float)it->img.w, h / (float)it->img.h);
        int tw = MAXI(1, (int)(it->img.w * scale)), th = MAXI(1, (int)(it->img.h * scale));
        if (img_create(&it->thumb, w, h)) {
            HBRUSH bg = CreateSolidBrush(theme()->thumb_back);
            FillRect(it->thumb.dc, &(RECT){0, 0, w, h}, bg);
            DeleteObject(bg);
            StretchBlt(it->thumb.dc, (w - tw) / 2, (h - th) / 2, tw, th, it->img.dc, 0, 0, it->img.w, it->img.h, SRCCOPY);
        }
        return;
    }
    if (img_create(&it->thumb, w, h)) {
        HBRUSH bg = CreateSolidBrush(theme()->thumb_back);
        FillRect(it->thumb.dc, &(RECT){0, 0, w, h}, bg);
        DeleteObject(bg);
        int x = 8, y = 10;
        WCHAR s[64];
        lstrcpynW(s, it->data ? it->data : L"", COUNTOF(s));
        RECT r = {x, y, w - x, h - y};
        HFONT f = S.font ? S.font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HGDIOBJ old = SelectObject(it->thumb.dc, f);
        SetTextColor(it->thumb.dc, theme()->text);
        SetBkMode(it->thumb.dc, TRANSPARENT);
        DrawTextW(it->thumb.dc, s, -1, &r, DT_LEFT | DT_TOP | DT_WORDBREAK);
        SelectObject(it->thumb.dc, old);
    }
}

static ShelfItem *item_new(ShType type, const WCHAR *data, int len)
{
    ShelfItem *it = mem_alloc(sizeof *it);
    if (!it)
        return NULL;
    it->type = type;
    it->len = len;
    if (data && len > 0) {
        it->data = mem_alloc(((size_t)len + 1) * sizeof(WCHAR));
        if (!it->data) {
            mem_free(it);
            return NULL;
        }
        memcpy(it->data, data, (size_t)len * sizeof(WCHAR));
    }
    make_thumb(it);
    return it;
}

static void item_free(ShelfItem *it)
{
    if (!it)
        return;
    if (it->img.bmp)
        img_destroy(&it->img);
    if (it->thumb.bmp)
        img_destroy(&it->thumb);
    if (it->data)
        mem_free(it->data);
    mem_free(it);
}

static void item_add(ShelfItem *it)
{
    if (!it)
        return;
    it->next = NULL;
    if (S.tail)
        S.tail->next = it;
    else
        S.head = it;
    S.tail = it;
    S.count++;
}

static void item_remove(ShelfItem *it)
{
    ShelfItem **p = &S.head;
    while (*p && *p != it)
        p = &(*p)->next;
    if (!*p)
        return;
    *p = it->next;
    if (S.tail == it) {
        ShelfItem *q = S.head;
        S.tail = NULL;
        while (q) {
            S.tail = q;
            q = q->next;
        }
    }
    S.count--;
    item_free(it);
}

/* ---- clipboard formats and helpers ------------------------------------------------ */

static void clip_put_file(HWND owner, const WCHAR *path)
{
    if (!OpenClipboard(owner))
        return;
    EmptyClipboard();
    int chars = lstrlenW(path) + 1;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, sizeof(DROPFILES) + (size_t)(chars + 1) * sizeof(WCHAR));
    if (h) {
        DROPFILES *df = GlobalLock(h);
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        df->pt.x = df->pt.y = 0;
        df->fNC = FALSE;
        memcpy((WCHAR *)((BYTE *)df + sizeof(DROPFILES)), path, (size_t)chars * sizeof(WCHAR));
        ((WCHAR *)((BYTE *)df + sizeof(DROPFILES)))[chars] = 0;
        GlobalUnlock(h);
        if (!SetClipboardData(CF_HDROP, h))
            GlobalFree(h);
    }
    CloseClipboard();
}

static HGLOBAL img_to_dib(const Img *im)
{
    if (!im->bmp || im->w <= 0 || im->h <= 0)
        return NULL;
    size_t stride = ((size_t)im->w * 4 + 3) & ~3;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + stride * (size_t)im->h);
    if (!h)
        return NULL;
    BITMAPINFOHEADER *bi = GlobalLock(h);
    memset(bi, 0, sizeof *bi);
    bi->biSize = sizeof *bi;
    bi->biWidth = im->w;
    bi->biHeight = im->h; /* positive: bottom-up, as CF_DIB expects */
    bi->biPlanes = 1;
    bi->biBitCount = 32;
    bi->biCompression = BI_RGB;
    bi->biSizeImage = (DWORD)(stride * im->h);
    BYTE *dst = (BYTE *)(bi + 1);
    for (int y = 0; y < im->h; y++)
        memcpy(dst + (size_t)(im->h - 1 - y) * stride, im->px + (size_t)y * im->w, (size_t)im->w * 4);
    GlobalUnlock(h);
    return h;
}

static bool is_svg_text(const WCHAR *s)
{
    if (!s)
        return false;
    if (wcsstr(s, L"<svg"))
        return true;
    return false;
}

static void item_copy(ShelfItem *it)
{
    if (!it)
        return;
    if (it->type == SH_IMAGE && it->img.bmp) {
        if (clip_put_image(S.daemon, it->img.px, it->img.w, it->img.w, it->img.h))
            return;
    }
    if (it->data) {
        if (it->type == SH_FILE || it->type == SH_VIDEO) {
            clip_put_file(S.daemon, it->data);
            return;
        }
        if (clip_put_text(S.daemon, it->data))
            return;
    }
    MessageBeep(MB_ICONWARNING);
}

/* ---- IDataObject / IDropSource for dragging items out ----------------------------- */

typedef struct ShObj ShObj;
typedef struct ShDS ShDS;

typedef struct {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(ShObj *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(ShObj *);
    ULONG(STDMETHODCALLTYPE *Release)(ShObj *);
    HRESULT(STDMETHODCALLTYPE *GetData)(ShObj *, FORMATETC *, STGMEDIUM *);
    HRESULT(STDMETHODCALLTYPE *GetDataHere)(ShObj *, FORMATETC *, STGMEDIUM *);
    HRESULT(STDMETHODCALLTYPE *QueryGetData)(ShObj *, FORMATETC *);
    HRESULT(STDMETHODCALLTYPE *GetCanonicalFormatEtc)(ShObj *, FORMATETC *, FORMATETC *);
    HRESULT(STDMETHODCALLTYPE *SetData)(ShObj *, FORMATETC *, STGMEDIUM *, BOOL);
    HRESULT(STDMETHODCALLTYPE *EnumFormatEtc)(ShObj *, DWORD, IEnumFORMATETC **);
    HRESULT(STDMETHODCALLTYPE *DAdvise)(ShObj *, FORMATETC *, DWORD, IAdviseSink *, DWORD *);
    HRESULT(STDMETHODCALLTYPE *DUnadvise)(ShObj *, DWORD);
    HRESULT(STDMETHODCALLTYPE *EnumDAdvise)(ShObj *, IEnumSTATDATA **);
} ShDOVtbl;

typedef struct {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(ShDS *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(ShDS *);
    ULONG(STDMETHODCALLTYPE *Release)(ShDS *);
    HRESULT(STDMETHODCALLTYPE *QueryContinueDrag)(ShDS *, BOOL, DWORD);
    HRESULT(STDMETHODCALLTYPE *GiveFeedback)(ShDS *, DWORD);
} ShDSVtbl;

struct ShDS {
    ShDSVtbl *vt;
    ShObj *owner;
};

struct ShObj {
    ShDOVtbl *vt; /* IDataObject view, first field */
    LONG ref;
    ShelfItem *item;
    UINT fmt_svg;
    ShDS ds;
};

typedef struct {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(IEnumFORMATETC *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(IEnumFORMATETC *);
    ULONG(STDMETHODCALLTYPE *Release)(IEnumFORMATETC *);
    HRESULT(STDMETHODCALLTYPE *Next)(IEnumFORMATETC *, ULONG, FORMATETC *, ULONG *);
    HRESULT(STDMETHODCALLTYPE *Skip)(IEnumFORMATETC *, ULONG);
    HRESULT(STDMETHODCALLTYPE *Reset)(IEnumFORMATETC *);
    HRESULT(STDMETHODCALLTYPE *Clone)(IEnumFORMATETC **);
} ShEnumVtbl;

typedef struct {
    ShEnumVtbl *vt;
    LONG ref;
    UINT fmt_svg;
    int pos, total;
} ShEnum;

static void fill_formats(FORMATETC *f, UINT svg, int i)
{
    f->ptd = NULL;
    f->dwAspect = DVASPECT_CONTENT;
    f->lindex = -1;
    f->tymed = TYMED_HGLOBAL;
    if (i == 0)
        f->cfFormat = CF_UNICODETEXT;
    else if (i == 1)
        f->cfFormat = CF_HDROP;
    else if (i == 2)
        f->cfFormat = CF_DIB;
    else
        f->cfFormat = svg;
}

static HRESULT STDMETHODCALLTYPE sh_enum_qi(IEnumFORMATETC *p, REFIID iid, void **out)
{
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IEnumFORMATETC)) {
        *out = p;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE sh_enum_add(IEnumFORMATETC *p) { return ++((ShEnum *)p)->ref; }
static ULONG STDMETHODCALLTYPE sh_enum_rel(IEnumFORMATETC *p)
{
    ULONG r = --((ShEnum *)p)->ref;
    if (!r)
        mem_free(p);
    return r;
}
static HRESULT STDMETHODCALLTYPE sh_enum_next(IEnumFORMATETC *p, ULONG c, FORMATETC *f, ULONG *got)
{
    ShEnum *e = (ShEnum *)p;
    ULONG n = 0;
    while (n < c && e->pos < e->total) {
        fill_formats(&f[n], e->fmt_svg, e->pos++);
        n++;
    }
    if (got)
        *got = n;
    return n == c ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE sh_enum_skip(IEnumFORMATETC *p, ULONG n)
{
    ShEnum *e = (ShEnum *)p;
    e->pos = MINI(e->pos + (int)n, e->total);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE sh_enum_reset(IEnumFORMATETC *p)
{
    ((ShEnum *)p)->pos = 0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE sh_enum_clone(IEnumFORMATETC **out)
{
    *out = NULL;
    return E_NOTIMPL;
}

static ShEnumVtbl kEnumVt = {sh_enum_qi, sh_enum_add, sh_enum_rel, sh_enum_next, sh_enum_skip, sh_enum_reset,
                                   sh_enum_clone};

static bool sh_supported(ShObj *o, UINT cf)
{
    return cf == CF_UNICODETEXT || cf == CF_HDROP || cf == CF_DIB || (o->fmt_svg && cf == o->fmt_svg);
}

static HGLOBAL sh_text_hglobal(const WCHAR *s, int len)
{
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, ((size_t)len + 1) * sizeof(WCHAR));
    if (!h)
        return NULL;
    WCHAR *d = GlobalLock(h);
    memcpy(d, s, (size_t)len * sizeof(WCHAR));
    d[len] = 0;
    GlobalUnlock(h);
    return h;
}

static HGLOBAL sh_file_hglobal(const WCHAR *path)
{
    int chars = lstrlenW(path) + 1;
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, sizeof(DROPFILES) + (size_t)(chars + 1) * sizeof(WCHAR));
    if (!h)
        return NULL;
    DROPFILES *df = GlobalLock(h);
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    df->pt.x = df->pt.y = 0;
    df->fNC = FALSE;
    WCHAR *d = (WCHAR *)((BYTE *)df + sizeof(DROPFILES));
    memcpy(d, path, (size_t)chars * sizeof(WCHAR));
    d[chars] = 0;
    GlobalUnlock(h);
    return h;
}

static HRESULT STDMETHODCALLTYPE sh_qi(ShObj *o, REFIID iid, void **out)
{
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDataObject)) {
        *out = o;
        return S_OK;
    }
    if (IsEqualIID(iid, &IID_IDropSource)) {
        *out = &o->ds;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE sh_add(ShObj *o) { return ++o->ref; }
static ULONG STDMETHODCALLTYPE sh_rel(ShObj *o)
{
    ULONG r = --o->ref;
    if (!r)
        mem_free(o);
    return r;
}
static HRESULT STDMETHODCALLTYPE sh_ds_qi(ShDS *d, REFIID iid, void **out) { return sh_qi(d->owner, iid, out); }
static ULONG STDMETHODCALLTYPE sh_ds_add(ShDS *d) { return sh_add(d->owner); }
static ULONG STDMETHODCALLTYPE sh_ds_rel(ShDS *d) { return sh_rel(d->owner); }
static HRESULT STDMETHODCALLTYPE sh_continue(ShDS *d, BOOL escape, DWORD keys)
{
    (void)d;
    if (escape)
        return DRAGDROP_S_CANCEL;
    if (!(keys & MK_LBUTTON))
        return DRAGDROP_S_DROP;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE sh_feedback(ShDS *d, DWORD effect)
{
    (void)d;
    return effect == DROPEFFECT_NONE ? S_OK : DRAGDROP_S_USEDEFAULTCURSORS;
}

static ShDSVtbl kDsVt = {sh_ds_qi, sh_ds_add, sh_ds_rel, sh_continue, sh_feedback};

static HRESULT STDMETHODCALLTYPE sh_get_data(ShObj *o, FORMATETC *f, STGMEDIUM *m)
{
    if (!f || !m)
        return E_INVALIDARG;
    if (!sh_supported(o, f->cfFormat))
        return DV_E_FORMATETC;
    HGLOBAL h = NULL;
    ShelfItem *it = o->item;
    if (f->cfFormat == CF_DIB && it->type == SH_IMAGE && it->img.bmp)
        h = img_to_dib(&it->img);
    else if (f->cfFormat == CF_HDROP && it->data)
        h = sh_file_hglobal(it->data);
    else if (it->data)
        h = sh_text_hglobal(it->data, it->len);
    if (!h)
        return E_OUTOFMEMORY;
    m->tymed = TYMED_HGLOBAL;
    m->hGlobal = h;
    m->pUnkForRelease = NULL;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE sh_get_data_here(ShObj *o, FORMATETC *f, STGMEDIUM *m)
{
    (void)o;
    (void)f;
    (void)m;
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE sh_query_data(ShObj *o, FORMATETC *f)
{
    if (!f || !sh_supported(o, f->cfFormat) || f->tymed != TYMED_HGLOBAL)
        return S_FALSE;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE sh_canonical(ShObj *o, FORMATETC *f, FORMATETC *out)
{
    (void)o;
    if (!f || !out)
        return E_INVALIDARG;
    *out = *f;
    out->ptd = NULL;
    return DATA_S_SAMEFORMATETC;
}
static HRESULT STDMETHODCALLTYPE sh_set_data(ShObj *o, FORMATETC *f, STGMEDIUM *m, BOOL release)
{
    (void)o;
    (void)f;
    (void)m;
    (void)release;
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE sh_enum_fmt(ShObj *o, DWORD dir, IEnumFORMATETC **out)
{
    *out = NULL;
    if (dir != DATADIR_GET)
        return E_NOTIMPL;
    ShEnum *e = mem_alloc(sizeof *e);
    if (!e)
        return E_OUTOFMEMORY;
    e->vt = &kEnumVt;
    e->ref = 1;
    e->fmt_svg = o->fmt_svg;
    e->total = 4;
    *out = (IEnumFORMATETC *)e;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE sh_advise(ShObj *o, FORMATETC *f, DWORD a, IAdviseSink *s, DWORD *c)
{
    (void)o;
    (void)f;
    (void)a;
    (void)s;
    (void)c;
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE sh_unadvise(ShObj *o, DWORD c)
{
    (void)o;
    (void)c;
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE sh_enum_advise(ShObj *o, IEnumSTATDATA **out)
{
    (void)o;
    *out = NULL;
    return E_NOTIMPL;
}

static ShDOVtbl kDoVt = {sh_qi, sh_add, sh_rel, sh_get_data, sh_get_data_here, sh_query_data, sh_canonical,
                               sh_set_data, sh_enum_fmt, sh_advise, sh_unadvise, sh_enum_advise};

static void drag_out(ShelfItem *it)
{
    ShObj *o = mem_alloc(sizeof *o);
    if (!o)
        return;
    o->vt = &kDoVt;
    o->ref = 1;
    o->item = it;
    o->fmt_svg = S.fmt_svg;
    o->ds.vt = &kDsVt;
    o->ds.owner = o;
    S.drag_item = it;
    DWORD effect = 0;
    DoDragDrop((IDataObject *)o, (IDropSource *)&o->ds, DROPEFFECT_COPY, &effect);
    sh_rel(o);
    S.drag_item = NULL;
}

/* ---- IDropTarget for dropping items onto the shelf -------------------------------- */

typedef struct {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(IDropTarget *, REFIID, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(IDropTarget *);
    ULONG(STDMETHODCALLTYPE *Release)(IDropTarget *);
    HRESULT(STDMETHODCALLTYPE *DragEnter)(IDropTarget *, IDataObject *, DWORD, POINTL, DWORD *);
    HRESULT(STDMETHODCALLTYPE *DragOver)(IDropTarget *, DWORD, POINTL, DWORD *);
    HRESULT(STDMETHODCALLTYPE *DragLeave)(IDropTarget *);
    HRESULT(STDMETHODCALLTYPE *Drop)(IDropTarget *, IDataObject *, DWORD, POINTL, DWORD *);
} ShTargetVtbl;

typedef struct {
    ShTargetVtbl *vt;
    LONG ref;
} ShTarget;

static HRESULT STDMETHODCALLTYPE sh_t_qi(IDropTarget *p, REFIID iid, void **out)
{
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDropTarget)) {
        *out = p;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE sh_t_add(IDropTarget *p) { return ++((ShTarget *)p)->ref; }
static ULONG STDMETHODCALLTYPE sh_t_rel(IDropTarget *p)
{
    ULONG r = --((ShTarget *)p)->ref;
    if (!r)
        mem_free(p);
    return r;
}
static HRESULT STDMETHODCALLTYPE sh_t_enter(IDropTarget *p, IDataObject *obj, DWORD keys, POINTL pt, DWORD *eff)
{
    (void)p;
    (void)obj;
    (void)keys;
    (void)pt;
    *eff = DROPEFFECT_COPY;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE sh_t_over(IDropTarget *p, DWORD keys, POINTL pt, DWORD *eff)
{
    (void)p;
    (void)keys;
    (void)pt;
    *eff = DROPEFFECT_COPY;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE sh_t_leave(IDropTarget *p)
{
    (void)p;
    return S_OK;
}

static void add_drop_data(IDataObject *obj)
{
    FORMATETC f = {CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM m = {0};
    if (SUCCEEDED(IDataObject_QueryGetData(obj, &f)) && SUCCEEDED(IDataObject_GetData(obj, &f, &m)) && m.hGlobal) {
        HDROP h = GlobalLock(m.hGlobal);
        UINT n = DragQueryFileW(h, 0xFFFFFFFF, NULL, 0);
        for (UINT i = 0; i < n; i++) {
            UINT len = DragQueryFileW(h, i, NULL, 0);
            WCHAR *path = mem_alloc(((size_t)len + 1) * sizeof(WCHAR));
            if (path) {
                DragQueryFileW(h, i, path, len + 1);
                shelf_add_file(path);
                mem_free(path);
            }
        }
        GlobalUnlock(m.hGlobal);
        ReleaseStgMedium(&m);
        return;
    }
    f.cfFormat = S.fmt_svg ? S.fmt_svg : CF_UNICODETEXT;
    memset(&m, 0, sizeof m);
    if (SUCCEEDED(IDataObject_GetData(obj, &f, &m)) && m.hGlobal) {
        WCHAR *text = GlobalLock(m.hGlobal);
        if (text) {
            ShelfItem *it = item_new(is_svg_text(text) ? SH_SVG : SH_TEXT, text, lstrlenW(text));
            item_add(it);
            GlobalUnlock(m.hGlobal);
        }
        ReleaseStgMedium(&m);
        return;
    }
    f.cfFormat = CF_DIB;
    memset(&m, 0, sizeof m);
    if (SUCCEEDED(IDataObject_GetData(obj, &f, &m)) && m.hGlobal) {
        BITMAPINFO *bi = GlobalLock(m.hGlobal);
        if (bi && bi->bmiHeader.biSize == sizeof(BITMAPINFOHEADER) && bi->bmiHeader.biWidth > 0) {
            int w = bi->bmiHeader.biWidth, bh = bi->bmiHeader.biHeight;
            int h = bh < 0 ? -bh : bh;
            bool bottom = bh > 0;
            Img im;
            if (img_create(&im, w, h)) {
                BITMAPINFO tmp = *bi;
                tmp.bmiHeader.biHeight = bottom ? h : -h;
                StretchDIBits(im.dc, 0, 0, w, h, 0, 0, w, h, (BYTE *)(bi + 1), &tmp, DIB_RGB_COLORS, SRCCOPY);
                shelf_add_image(&im);
                img_destroy(&im);
            }
        }
        if (bi)
            GlobalUnlock(m.hGlobal);
        ReleaseStgMedium(&m);
    }
}

static HRESULT STDMETHODCALLTYPE sh_t_drop(IDropTarget *p, IDataObject *obj, DWORD keys, POINTL pt, DWORD *eff)
{
    (void)p;
    (void)keys;
    (void)pt;
    *eff = DROPEFFECT_NONE;
    if (!obj)
        return E_INVALIDARG;
    add_drop_data(obj);
    redraw();
    return S_OK;
}

static ShTargetVtbl kTargetVt = {sh_t_qi, sh_t_add, sh_t_rel, sh_t_enter, sh_t_over, sh_t_leave, sh_t_drop};

/* ---- public add functions ---------------------------------------------------------- */

bool shelf_add_image(Img *im)
{
    if (!im || !im->bmp)
        return false;
    ShelfItem *it = mem_alloc(sizeof *it);
    if (!it)
        return false;
    it->type = SH_IMAGE;
    if (!img_create(&it->img, im->w, im->h)) {
        mem_free(it);
        return false;
    }
    memcpy(it->img.px, im->px, (size_t)im->w * im->h * 4);
    item_add(it);
    make_thumb(it);
    redraw();
    return true;
}

bool shelf_add_text(const WCHAR *text, int len)
{
    if (!text)
        return false;
    if (len < 0)
        len = lstrlenW(text);
    ShelfItem *it = item_new(is_svg_text(text) ? SH_SVG : SH_TEXT, text, len);
    item_add(it);
    redraw();
    return it != NULL;
}

bool shelf_add_file(const WCHAR *path)
{
    if (!path || !path[0])
        return false;
    static const WCHAR *video[] = {L".mp4", L".mkv", L".avi", L".mov", L".webm", L".wmv", NULL};
    ShType type = SH_FILE;
    const WCHAR *dot = wcsrchr(path, L'.');
    if (dot)
        for (int i = 0; video[i]; i++)
            if (!lstrcmpiW(dot, video[i]))
                type = SH_VIDEO;
    if (dot && !lstrcmpiW(dot, L".svg"))
        type = SH_SVG;
    ShelfItem *it = item_new(type, path, lstrlenW(path));
    item_add(it);
    redraw();
    return it != NULL;
}

bool shelf_add_files(const WCHAR *list)
{
    const WCHAR *p = list;
    bool any = false;
    while (*p) {
        if (shelf_add_file(p))
            any = true;
        p += lstrlenW(p) + 1;
    }
    return any;
}

bool shelf_add_clipboard(HWND hwnd)
{
    /* Text, SVG and files keep their original form so they can be dragged into
     * design software; only a real bitmap is rendered into an image item. */
    if (!OpenClipboard(hwnd))
        return false;
    bool ok = false;
    if (IsClipboardFormatAvailable(S.fmt_svg) || IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        UINT fmt = IsClipboardFormatAvailable(S.fmt_svg) ? S.fmt_svg : CF_UNICODETEXT;
        HGLOBAL h = GetClipboardData(fmt);
        WCHAR *t = h ? GlobalLock(h) : NULL;
        if (t) {
            ok = shelf_add_text(t, lstrlenW(t));
            GlobalUnlock(h);
        }
    } else if (IsClipboardFormatAvailable(CF_HDROP)) {
        HDROP h = GetClipboardData(CF_HDROP);
        UINT n = h ? DragQueryFileW(h, 0xFFFFFFFF, NULL, 0) : 0;
        for (UINT i = 0; i < n; i++) {
            WCHAR path[MAX_PATH];
            DragQueryFileW(h, i, path, COUNTOF(path));
            if (shelf_add_file(path))
                ok = true;
        }
    }
    CloseClipboard();
    if (ok)
        return true;
    Img im;
    if (clip_get_image(hwnd, &im) == EXIT_OK) {
        ok = shelf_add_image(&im);
        img_destroy(&im);
    }
    return ok;
}

bool shelf_add_capture(Img *im) { return shelf_add_image(im); }

/* ---- window ----------------------------------------------------------------------- */

static void bar_geometry(RECT *r)
{
    int h = S.expanded ? OPEN_H : BAR_H;
    int w = MINI(GetSystemMetrics(SM_CXVIRTUALSCREEN) - 40, 960);
    if (w < 320)
        w = 320;
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN) + (GetSystemMetrics(SM_CXVIRTUALSCREEN) - w) / 2;
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN) + 4;
    SetRect(r, x, y, x + w, y + h);
}

static void set_expanded(HWND bar, bool expanded)
{
    if (!bar || expanded == S.expanded)
        return;
    S.expanded = expanded;
    if (!expanded)
        S.hover_index = 0;
    RECT r;
    bar_geometry(&r);
    SetWindowPos(bar, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
    InvalidateRect(bar, NULL, TRUE);
}

/* 1-based index into the list; 0 means no item. Avoids truncating pointers. */
static int item_at(POINT pt)
{
    if (!S.expanded)
        return 0;
    RECT r;
    GetClientRect(S.bar, &r);
    int cell = THUMB_W + 10;
    int x = 12, i = 1;
    for (ShelfItem *it = S.head; it && x + THUMB_W <= r.right; it = it->next, i++) {
        if (pt.x >= x && pt.x < x + THUMB_W && pt.y >= BAR_H + 8 && pt.y < BAR_H + 8 + THUMB_H)
            return i;
        x += cell;
    }
    return 0;
}

static ShelfItem *item_by_index(int index)
{
    ShelfItem *it = S.head;
    for (int i = 1; it && i < index; i++)
        it = it->next;
    return it;
}

static LRESULT CALLBACK bar_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    const Theme *t = theme();
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        HBRUSH bg = CreateSolidBrush(t->bar);
        FillRect(dc, &r, bg);
        DeleteObject(bg);
        HPEN edge = CreatePen(PS_SOLID, 1, t->edge);
        HGDIOBJ old = SelectObject(dc, edge);
        MoveToEx(dc, 0, BAR_H - 1, NULL);
        LineTo(dc, r.right, BAR_H - 1);
        SelectObject(dc, old);
        DeleteObject(edge);
        SetTextColor(dc, t->text);
        SetBkMode(dc, TRANSPARENT);
        if (S.font)
            SelectObject(dc, S.font);
        WCHAR label[96];
        wsprintfW(label, L"%s (%d) — %s", T(SH_MENU), S.count, T(SH_HINT));
        TextOutW(dc, 12, 12, label, lstrlenW(label));
        if (S.expanded) {
            int x = 12, y = BAR_H + 8, i = 1;
            for (ShelfItem *it = S.head; it && x + THUMB_W < r.right; it = it->next, i++) {
                RECT box = {x, y, x + THUMB_W, y + THUMB_H};
                HBRUSH tb = CreateSolidBrush(t->thumb_back);
                FillRect(dc, &box, tb);
                DeleteObject(tb);
                if (it->thumb.bmp)
                    BitBlt(dc, x, y, THUMB_W, THUMB_H, it->thumb.dc, 0, 0, SRCCOPY);
                bool hot = i == S.hover_index;
                HPEN p = CreatePen(PS_SOLID, hot ? 2 : 1, hot ? t->accent : t->edge);
                HGDIOBJ op = SelectObject(dc, p);
                Rectangle(dc, x, y, x + THUMB_W, y + THUMB_H);
                SelectObject(dc, op);
                DeleteObject(p);
                x += THUMB_W + 10;
            }
            if (!S.head) {
                RECT tr = {12, y, r.right - 12, y + THUMB_H};
                DrawTextW(dc, T(SH_EMPTY), -1, &tr, DT_SINGLELINE | DT_VCENTER);
            }
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (S.down_active && S.down_item) {
            int dx = pt.x - S.down_pt.x, dy = pt.y - S.down_pt.y;
            if (dx * dx + dy * dy >= 25) { /* a real drag, not a click */
                ShelfItem *it = S.down_item;
                S.down_active = false;
                S.down_item = NULL;
                if (GetCapture() == h)
                    ReleaseCapture();
                drag_out(it);
                return 0;
            }
        }
        int idx = item_at(pt);
        if (idx != S.hover_index) {
            S.hover_index = idx;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (S.down_active) {
            if (GetCapture() == h)
                ReleaseCapture();
            if (S.down_item)
                item_copy(S.down_item);
            S.down_active = false;
            S.down_item = NULL;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    case WM_CANCELMODE:
        S.down_active = false;
        S.down_item = NULL;
        return 0;
    case WM_RBUTTONUP: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int idx = item_at(pt);
        HMENU menu = CreatePopupMenu();
        if (idx > 0) {
            ShelfItem *it = item_by_index(idx);
            AppendMenuW(menu, MF_STRING, 1, T(SH_COPY));
            AppendMenuW(menu, MF_STRING, 2, T(SH_REMOVE));
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            S.drag_item = it;
        }
        AppendMenuW(menu, MF_STRING, 3, T(SH_CLEAR));
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        HMENU themes = CreateMenu();
        for (int i = 0; i < SH_THEME_COUNT; i++)
            AppendMenuW(themes, MF_STRING | (i == S.theme ? MF_CHECKED : 0), 10 + i, T(SH_THEME0 + i));
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)themes, T(SH_THEME));
        ClientToScreen(h, &pt);
        SetForegroundWindow(h);
        UINT c = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, h, NULL);
        PostMessageW(h, WM_NULL, 0, 0);
        DestroyMenu(menu);
        if (c == 1 && S.drag_item)
            item_copy(S.drag_item);
        else if (c == 2 && S.drag_item) {
            item_remove(S.drag_item);
            S.hover_index = 0;
            InvalidateRect(h, NULL, TRUE);
        } else if (c == 3) {
            while (S.head) {
                ShelfItem *n = S.head->next;
                item_free(S.head);
                S.head = n;
            }
            S.tail = NULL;
            S.count = 0;
            S.hover_index = 0;
            InvalidateRect(h, NULL, TRUE);
        } else if (c >= 10 && c < 10 + SH_THEME_COUNT) {
            shelf_set_theme(c - 10);
        }
        S.drag_item = NULL;
        return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int idx = item_at(pt);
        if (idx > 0) {
            S.down_index = idx;
            S.down_pt = pt;
            S.down_item = item_by_index(idx);
            S.down_active = true;
            SetCapture(h);
        } else if (!S.expanded) {
            S.pinned = !S.pinned;
            set_expanded(h, S.pinned);
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == SH_HOVER_TIMER) {
            POINT pt;
            GetCursorPos(&pt);
            RECT r;
            bar_geometry(&r);
            bool is_near = rect_has(&r, pt.x, pt.y);
            bool want = S.pinned || (S.always && is_near);
            set_expanded(h, want);
        }
        return 0;
    case WM_SIZE:
        InvalidateRect(h, NULL, TRUE);
        return 0;
    case WM_DESTROY:
        RevokeDragDrop(h);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

/* ---- public shelf API -------------------------------------------------------------- */

static bool shelf_create(void)
{
    if (S.bar)
        return true;
    HWND bar = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"sniplite.shelf", T(SH_MENU),
                               WS_POPUP | WS_CLIPSIBLINGS, 0, 0, 10, 10, S.daemon, NULL, S.inst, NULL);
    if (!bar)
        return false;
    S.bar = bar;
    S.visible = true;
    RECT r;
    bar_geometry(&r);
    SetWindowPos(bar, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
    ShowWindow(bar, SW_SHOWNOACTIVATE);
    SetTimer(bar, SH_HOVER_TIMER, 120, NULL);
    ShTarget *target = mem_alloc(sizeof *target);
    if (target) {
        target->vt = &kTargetVt;
        target->ref = 1;
        RegisterDragDrop(bar, (IDropTarget *)target);
    }
    return true;
}

static bool shelf_enable(void)
{
    Config c;
    config_load(&c);
    if (!c.shelf_on) {
        c.shelf_on = true;
        config_save(&c);
    }
    S.theme = c.shelf_theme;
    S.always = c.shelf_always;
    return shelf_create();
}

void shelf_apply_config(void)
{
    Config c;
    config_load(&c);
    S.theme = c.shelf_theme;
    S.always = c.shelf_always;
    if (c.shelf_on)
        shelf_create();
    else if (S.bar) {
        KillTimer(S.bar, SH_HOVER_TIMER);
        DestroyWindow(S.bar);
        S.bar = NULL;
        S.visible = false;
        S.expanded = false;
        S.pinned = false;
    }
    if (S.bar)
        redraw();
}

void shelf_show(bool show)
{
    if (!S.bar && !shelf_enable())
        return;
    ShowWindow(S.bar, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    S.visible = show;
}

void shelf_toggle(void)
{
    if (!S.bar && !shelf_enable())
        return;
    S.pinned = !S.pinned;
    if (S.expanded != S.pinned)
        set_expanded(S.bar, S.pinned);
    else
        redraw();
}

bool shelf_visible(void) { return S.visible; }
void shelf_set_theme(int theme)
{
    S.theme = CLAMPI(theme, 0, SH_THEME_COUNT - 1);
    Config c;
    config_load(&c);
    c.shelf_theme = S.theme;
    config_save(&c);
    redraw();
}

int shelf_count(void) { return S.count; }

int shelf_cmd(int argc, WCHAR **argv, bool json, Str *out)
{
    (void)json;
    const WCHAR *c = argc > 1 ? argv[1] : L"show";
    if (arg_eq(c, L"show"))
        shelf_show(true);
    else if (arg_eq(c, L"hide"))
        shelf_show(false);
    else if (arg_eq(c, L"toggle"))
        shelf_toggle();
    else if (arg_eq(c, L"clear")) {
        while (S.head) {
            ShelfItem *n = S.head->next;
            item_free(S.head);
            S.head = n;
        }
        S.tail = NULL;
        S.count = 0;
        str_add(out, T(SH_CLEARED));
    } else if (arg_eq(c, L"add")) {
        if (!shelf_add_clipboard(S.daemon))
            str_add(out, T(CLIP_EMPTY));
    } else if (arg_eq(c, L"count")) {
        WCHAR n[32];
        wsprintfW(n, L"%d\n", S.count);
        str_add(out, n);
    } else if (arg_eq(c, L"file") && argc > 2) {
        if (!shelf_add_file(argv[2]))
            str_add(out, T(CAP_FAILED));
    } else {
        str_add(out, T(SH_USAGE));
        return EXIT_USAGE;
    }
    return EXIT_OK;
}

bool shelf_init(HINSTANCE inst, HWND daemon)
{
    S.inst = inst;
    S.daemon = daemon;
    S.fmt_svg = RegisterClipboardFormatW(L"text/svg+xml");
    OleInitialize(NULL);
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = bar_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"sniplite.shelf";
    if (!RegisterClassW(&wc))
        return false;
    S.font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, T(FONT));
    shelf_apply_config();
    return true;
}

void shelf_shutdown(void)
{
    while (S.head) {
        ShelfItem *n = S.head->next;
        item_free(S.head);
        S.head = n;
    }
    S.tail = NULL;
    S.count = 0;
    if (S.bar) {
        KillTimer(S.bar, SH_HOVER_TIMER);
        DestroyWindow(S.bar);
        S.bar = NULL;
    }
    if (S.font)
        DeleteObject(S.font);
    OleUninitialize();
}
