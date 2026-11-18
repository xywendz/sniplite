/* Export pages created by the studio. WIC writes JPEG (also embedded into PDF
 * and PPTX), libwebp is loaded on demand, and the PDF/PPTX containers are
 * written directly: no extra DLL or runtime is needed for those two. */
#include "feature.h"
#include "image.h"
#include "lang.h"
#include <stdarg.h>
#include <objidl.h>
#include <wincodec.h>

/* ---- growable byte buffer --------------------------------------------------------- */

typedef struct {
    uint8_t *p;
    size_t n, cap;
} Buf;

static bool buf_reserve(Buf *b, size_t extra)
{
    if (b->n + extra <= b->cap)
        return true;
    size_t cap = b->cap ? b->cap * 2 : 4096;
    while (cap < b->n + extra)
        cap *= 2;
    uint8_t *p = mem_grow(b->p, cap);
    if (!p)
        return false;
    b->p = p;
    b->cap = cap;
    return true;
}

static bool buf_add(Buf *b, const void *data, size_t len)
{
    if (!len)
        return true;
    if (!buf_reserve(b, len))
        return false;
    memcpy(b->p + b->n, data, len);
    b->n += len;
    return true;
}

static int cstr_len(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

static bool buf_str(Buf *b, const char *s) { return buf_add(b, s, (size_t)cstr_len(s)); }

/* wsprintf has no floating point support: render fixed-point numbers by hand. */
static const char *fixed2(double v, char *out)
{
    int x = (int)(v * 100 + 0.5);
    wsprintfA(out, "%d.%02d", x / 100, x % 100);
    return out;
}

static bool buf_fmt(Buf *b, const char *fmt, ...)
{
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = wvsprintfA(tmp, fmt, ap);
    va_end(ap);
    return n > 0 && buf_add(b, tmp, (size_t)n);
}

/* ---- JPEG encoder with quality (temp file, read back) ----------------------------- */

static void temp_path(WCHAR *out, int cap, int index, const WCHAR *ext)
{
    WCHAR dir[MAX_PATH];
    GetTempPathW(COUNTOF(dir), dir);
    wsprintfW(out, L"%ssniplite-%u-%d%s", dir, GetCurrentProcessId(), index, ext);
    (void)cap;
}

static bool wic_save_jpeg(const WCHAR *path, const uint32_t *px, int w, int h, int quality)
{
    IWICImagingFactory *fac = NULL;
    if (FAILED(com_create(&CLSID_WICImagingFactory, &IID_IWICImagingFactory, (void **)&fac)))
        return false;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IWICBitmap *bmp = NULL;
    IPropertyBag2 *bag = NULL;
    bool ok = false;
    do {
        if (FAILED(IWICImagingFactory_CreateStream(fac, &stream)))
            break;
        if (FAILED(IWICStream_InitializeFromFilename(stream, path, GENERIC_WRITE)))
            break;
        if (FAILED(IWICImagingFactory_CreateEncoder(fac, &GUID_ContainerFormatJpeg, NULL, &enc)))
            break;
        if (FAILED(IWICBitmapEncoder_Initialize(enc, (IStream *)stream, WICBitmapEncoderNoCache)))
            break;
        if (FAILED(IWICBitmapEncoder_CreateNewFrame(enc, &frame, &bag)))
            break;
        if (bag) {
            PROPBAG2 pb = {0};
            pb.pstrName = (LPOLESTR)L"Quality";
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_R4;
            v.fltVal = (float)CLAMPI(quality, 10, 100) / 100.0f;
            IPropertyBag2_Write(bag, 1, &pb, &v);
        }
        if (FAILED(IWICBitmapFrameEncode_Initialize(frame, bag)))
            break;
        if (FAILED(IWICImagingFactory_CreateBitmapFromMemory(fac, (UINT)w, (UINT)h, &GUID_WICPixelFormat32bppBGRA,
                                                            (UINT)w * 4, (size_t)w * 4 * (size_t)h, (BYTE *)px, &bmp)))
            break;
        if (FAILED(IWICBitmapFrameEncode_WriteSource(frame, (IWICBitmapSource *)bmp, NULL)))
            break;
        if (FAILED(IWICBitmapFrameEncode_Commit(frame)) || FAILED(IWICBitmapEncoder_Commit(enc)))
            break;
        ok = true;
    } while (0);
    if (bmp)
        IWICBitmap_Release(bmp);
    if (bag)
        IPropertyBag2_Release(bag);
    if (frame)
        IWICBitmapFrameEncode_Release(frame);
    if (enc)
        IWICBitmapEncoder_Release(enc);
    if (stream)
        IWICStream_Release(stream);
    IWICImagingFactory_Release(fac);
    return ok;
}

static bool read_file(const WCHAR *path, Buf *out)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    DWORD size = GetFileSize(f, NULL);
    if (size == INVALID_FILE_SIZE || !buf_reserve(out, size)) {
        CloseHandle(f);
        return false;
    }
    DWORD got = 0;
    bool ok = ReadFile(f, out->p + out->n, size, &got, NULL) && got == size;
    if (ok)
        out->n += got;
    CloseHandle(f);
    DeleteFileW(path);
    return ok;
}

static bool jpeg_bytes(const ExportPage *p, int quality, int slot, Buf *out)
{
    WCHAR tmp[MAX_PATH];
    temp_path(tmp, COUNTOF(tmp), slot, L".jpg");
    if (!wic_save_jpeg(tmp, p->px, p->w, p->h, quality))
        return false;
    return read_file(tmp, out);
}

/* ---- WEBP via libwebp, loaded on demand ------------------------------------------- */

typedef size_t(WINAPI *PFN_WebPEncodeBGRA)(const uint8_t *, int, int, int, float, uint8_t **);
typedef void(WINAPI *PFN_WebPFree)(void *);

static bool webp_encode(const ExportPage *page, int quality, Buf *out, Str *err)
{
    HMODULE lib = LoadLibraryW(L"libwebp.dll");
    if (!lib)
        lib = LoadLibraryW(L"webp.dll");
    if (!lib) {
        if (err)
            str_add(err, T(STU_WEBP_MISSING));
        return false;
    }
    PFN_WebPEncodeBGRA enc = (PFN_WebPEncodeBGRA)(void *)GetProcAddress(lib, "WebPEncodeBGRA");
    PFN_WebPFree freefn = (PFN_WebPFree)(void *)GetProcAddress(lib, "WebPFree");
    bool ok = false;
    if (enc && freefn) {
        uint8_t *data = NULL;
        size_t n = enc((const uint8_t *)page->px, page->w, page->h, page->w * 4, (float)quality, &data);
        if (n && data) {
            ok = buf_add(out, data, n);
            freefn(data);
        }
    } else if (err) {
        str_add(err, T(STU_WEBP_MISSING));
    }
    FreeLibrary(lib);
    return ok;
}

bool export_write_file(const WCHAR *path, const void *data, int len)
{
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    DWORD wrote = 0;
    bool ok = WriteFile(f, data, (DWORD)len, &wrote, NULL) && wrote == (DWORD)len;
    CloseHandle(f);
    return ok;
}

static void split_name(const WCHAR *path, WCHAR *base, int cap, WCHAR *ext, int ecap)
{
    lstrcpynW(base, path, cap);
    ext[0] = 0;
    WCHAR *dot = wcsrchr(base, L'.'), *sl = wcsrchr(base, L'\\');
    if (dot && (!sl || dot > sl)) {
        lstrcpynW(ext, dot, ecap);
        *dot = 0;
    }
}

static bool write_images(const WCHAR *path, int fmt, int quality, ExportPage *pages, int count, Str *err)
{
    WCHAR base[MAX_PATH * 2], ext[32];
    split_name(path, base, COUNTOF(base), ext, COUNTOF(ext));
    for (int i = 0; i < count; i++) {
        WCHAR out[MAX_PATH * 2];
        if (count == 1)
            lstrcpynW(out, path, COUNTOF(out));
        else
            wsprintfW(out, L"%s_%d%s", base, i + 1, ext);
        if (fmt == 4 || fmt == 3) {
            Buf b = {0};
            bool ok = fmt == 4 ? jpeg_bytes(&pages[i], quality, i, &b) : webp_encode(&pages[i], quality, &b, err);
            if (ok)
                ok = export_write_file(out, b.p, (int)b.n);
            mem_free(b.p);
            if (!ok) {
                if (err && fmt == 4)
                    str_addf(err, T(STU_FAIL), out);
                return false;
            }
        } else if (!image_save(out, pages[i].px, pages[i].w, pages[i].w, pages[i].h)) {
            if (err)
                str_addf(err, T(STU_FAIL), out);
            return false;
        }
    }
    return true;
}

/* ---- PDF -------------------------------------------------------------------------- */

static uint32_t crc_table[256];
static bool crc_ready;

static void crc_init(void)
{
    if (crc_ready)
        return;
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
    crc_ready = true;
}

static uint32_t crc32(const void *data, size_t len)
{
    crc_init();
    const uint8_t *p = data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static bool write_pdf(const WCHAR *path, ExportPage *pages, int count, int quality, Str *err)
{
    const double pw = 595.28, ph = 841.89, margin = 20.0;
    bool ok = false;
    if (count > 64) {
        if (err)
            str_add(err, T(STU_FAIL));
        return false;
    }
    int n = count;
    Buf jpgs[64];
    memset(jpgs, 0, sizeof jpgs);
    for (int i = 0; i < n; i++)
        if (!jpeg_bytes(&pages[i], quality, i, &jpgs[i])) {
            if (err)
                str_add(err, T(STU_FAIL));
            goto fail;
        }

    int page0 = 3, content0 = 3 + n, image0 = 3 + 2 * n;
    int total = 2 + 3 * n;
    size_t *offs = mem_alloc((size_t)(total + 2) * sizeof *offs);
    Buf out = {0};
    if (!offs)
        goto fail;
    memset(offs, 0, (size_t)(total + 2) * sizeof *offs);

    buf_str(&out, "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n");
    offs[1] = out.n;
    buf_str(&out, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    offs[2] = out.n;
    buf_str(&out, "2 0 obj\n<< /Type /Pages /Kids [");
    for (int i = 0; i < n; i++)
        buf_fmt(&out, "%d 0 R ", page0 + i);
    buf_fmt(&out, "] /Count %d >>\nendobj\n", n);

    char spw[32], sph[32];
    fixed2(pw, spw);
    fixed2(ph, sph);
    for (int i = 0; i < n; i++) {
        int po = page0 + i, co = content0 + i, xo = image0 + i;
        double scale = MINI((pw - 2 * margin) / pages[i].w, (ph - 2 * margin) / pages[i].h);
        double dw = pages[i].w * scale, dh = pages[i].h * scale;
        char sdw[32], sdh[32], sox[32], soy[32];
        fixed2(dw, sdw);
        fixed2(dh, sdh);
        fixed2((pw - dw) / 2, sox);
        fixed2((ph - dh) / 2, soy);
        offs[po] = out.n;
        buf_fmt(&out, "%d 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %s %s] /Resources << /XObject << /Im0 %d 0 R >> >> /Contents %d 0 R >>\nendobj\n",
                po, spw, sph, xo, co);
        char stream[256];
        int sl = wsprintfA(stream, "q\n%s 0 0 %s %s %s cm\n/Im0 Do\nQ\n", sdw, sdh, sox, soy);
        offs[co] = out.n;
        buf_fmt(&out, "%d 0 obj\n<< /Length %d >>\nstream\n%s\nendstream\nendobj\n", co, sl, stream);
    }
    for (int i = 0; i < n; i++) {
        int xo = image0 + i;
        offs[xo] = out.n;
        buf_fmt(&out, "%d 0 obj\n<< /Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length %d >>\nstream\n",
                xo, pages[i].w, pages[i].h, (int)jpgs[i].n);
        buf_add(&out, jpgs[i].p, jpgs[i].n);
        buf_str(&out, "\nendstream\nendobj\n");
    }

    size_t xref = out.n;
    buf_fmt(&out, "xref\n0 %d\n0000000000 65535 f \n", total + 1);
    for (int i = 1; i <= total; i++)
        buf_fmt(&out, "%010u 00000 n \n", (unsigned)offs[i]);
    buf_fmt(&out, "trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%u\n%%%%EOF\n", total + 1, (unsigned)xref);
    ok = export_write_file(path, out.p, (int)out.n);
    mem_free(offs);
    mem_free(out.p);
fail:
    for (int i = 0; i < n; i++)
        mem_free(jpgs[i].p);
    if (!ok && err)
        str_addf(err, T(STU_FAIL), path);
    return ok;
}

/* ---- ZIP (store) + PPTX ------------------------------------------------------------ */

typedef struct {
    WCHAR name[128];
    uint32_t crc, size, offset;
    uint16_t time, date;
} ZipEntry;

static void zip_add(Buf *zip, ZipEntry *e, const void *data, size_t len, const WCHAR *name)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    e->time = (uint16_t)((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
    e->date = (uint16_t)(((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay);
    lstrcpynW(e->name, name, COUNTOF(e->name));
    e->crc = crc32(data, len);
    e->size = (uint32_t)len;
    e->offset = (uint32_t)zip->n;
    char n[260];
    int nn = WideCharToMultiByte(CP_UTF8, 0, name, -1, n, sizeof n, NULL, NULL);
    if (nn > 0)
        nn--;
    uint8_t h[30] = {0};
    h[0] = 0x50; h[1] = 0x4B; h[2] = 0x03; h[3] = 0x04;
    h[4] = 20;
    memcpy(h + 10, &e->time, 2);
    memcpy(h + 12, &e->date, 2);
    memcpy(h + 14, &e->crc, 4);
    memcpy(h + 18, &e->size, 4);
    memcpy(h + 22, &e->size, 4);
    h[26] = (uint8_t)nn;
    h[27] = (uint8_t)(nn >> 8);
    buf_add(zip, h, sizeof h);
    buf_add(zip, n, (size_t)nn);
    buf_add(zip, data, len);
}

static void zip_text(Buf *zip, ZipEntry *e, const WCHAR *name, Buf *text)
{
    zip_add(zip, e, text->p, text->n, name);
    mem_free(text->p);
    text->p = NULL;
    text->n = text->cap = 0;
}

static void zip_finish(Buf *zip, ZipEntry *entries, int count)
{
    size_t cd = zip->n;
    for (int i = 0; i < count; i++) {
        ZipEntry *e = &entries[i];
        char n[260];
        int nn = WideCharToMultiByte(CP_UTF8, 0, e->name, -1, n, sizeof n, NULL, NULL);
        if (nn > 0)
            nn--;
        uint8_t h[46] = {0};
        h[0] = 0x50; h[1] = 0x4B; h[2] = 0x01; h[3] = 0x02;
        h[4] = 20; h[5] = 20;
        memcpy(h + 12, &e->time, 2);
        memcpy(h + 14, &e->date, 2);
        memcpy(h + 16, &e->crc, 4);
        memcpy(h + 20, &e->size, 4);
        memcpy(h + 24, &e->size, 4);
        h[28] = (uint8_t)nn;
        h[29] = (uint8_t)(nn >> 8);
        memcpy(h + 42, &e->offset, 4);
        buf_add(zip, h, sizeof h);
        buf_add(zip, n, (size_t)nn);
    }
    uint16_t count16 = (uint16_t)count;
    uint32_t sz = (uint32_t)(zip->n - cd), off = (uint32_t)cd;
    uint8_t end[22] = {0};
    end[0] = 0x50; end[1] = 0x4B; end[2] = 0x05; end[3] = 0x06;
    memcpy(end + 8, &count16, 2);
    memcpy(end + 10, &count16, 2);
    memcpy(end + 12, &sz, 4);
    memcpy(end + 16, &off, 4);
    buf_add(zip, end, sizeof end);
}

static bool write_pptx(const WCHAR *path, ExportPage *pages, int count, int quality, Str *err)
{
    if (count > 64) {
        if (err)
            str_add(err, T(STU_FAIL));
        return false;
    }
    int n = count;
    ZipEntry *e = mem_alloc((size_t)(8 + 3 * n) * sizeof *e);
    Buf zip = {0};
    bool ok = false;
    if (!e)
        return false;
    memset(e, 0, (size_t)(8 + 3 * n) * sizeof *e);
    Buf t = {0};

    buf_str(&t, "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"><Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/><Default Extension=\"xml\" ContentType=\"application/xml\"/><Default Extension=\"jpeg\" ContentType=\"image/jpeg\"/><Override PartName=\"/ppt/presentation.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml\"/><Override PartName=\"/ppt/slideMasters/slideMaster1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slideMaster+xml\"/><Override PartName=\"/ppt/slideLayouts/slideLayout1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slideLayout+xml\"/>");
    for (int i = 1; i <= n; i++)
        buf_fmt(&t, "<Override PartName=\"/ppt/slides/slide%d.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slide+xml\"/>", i);
    buf_str(&t, "</Types>");
    zip_text(&zip, &e[0], L"[Content_Types].xml", &t);

    t = (Buf){0};
    buf_str(&t, "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"ppt/presentation.xml\"/></Relationships>");
    zip_text(&zip, &e[1], L"_rels/.rels", &t);

    t = (Buf){0};
    buf_str(&t, "<?xml version=\"1.0\" encoding=\"UTF-8\"?><p:presentation xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"><p:sldMasterIdLst><p:sldMasterId id=\"2147483648\" r:id=\"rId1\"/></p:sldMasterIdLst><p:sldIdLst>");
    for (int i = 1; i <= n; i++)
        buf_fmt(&t, "<p:sldId id=\"%d\" r:id=\"rId%d\"/>", 255 + i, 1 + i);
    buf_str(&t, "</p:sldIdLst><p:sldSz cx=\"9144000\" cy=\"6858000\"/><p:notesSz cx=\"6858000\" cy=\"9144000\"/></p:presentation>");
    zip_text(&zip, &e[2], L"ppt/presentation.xml", &t);

    t = (Buf){0};
    buf_str(&t, "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster\" Target=\"slideMasters/slideMaster1.xml\"/>");
    for (int i = 1; i <= n; i++)
        buf_fmt(&t, "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide\" Target=\"slides/slide%d.xml\"/>", 1 + i, i);
    buf_str(&t, "</Relationships>");
    zip_text(&zip, &e[3], L"ppt/_rels/presentation.xml.rels", &t);

    t = (Buf){0};
    buf_str(&t, "<?xml version=\"1.0\" encoding=\"UTF-8\"?><p:sldMaster xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"><p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/></p:spTree></p:cSld><p:clrMap bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" tx2=\"dk2\" accent1=\"accent1\" accent2=\"accent2\" accent3=\"accent3\" accent4=\"accent4\" accent5=\"accent5\" accent6=\"accent6\" hlink=\"hlink\" folHlink=\"folHlink\"/><p:sldLayoutIdLst><p:sldLayoutId id=\"2147483649\" r:id=\"rId1\"/></p:sldLayoutIdLst></p:sldMaster>");
    zip_text(&zip, &e[4], L"ppt/slideMasters/slideMaster1.xml", &t);

    t = (Buf){0};
    buf_str(&t, "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout\" Target=\"../slideLayouts/slideLayout1.xml\"/></Relationships>");
    zip_text(&zip, &e[5], L"ppt/slideMasters/_rels/slideMaster1.xml.rels", &t);

    t = (Buf){0};
    buf_str(&t, "<?xml version=\"1.0\" encoding=\"UTF-8\"?><p:sldLayout xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\" type=\"blank\" preserve=\"1\"><p:cSld name=\"Blank\"><p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/></p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sldLayout>");
    zip_text(&zip, &e[6], L"ppt/slideLayouts/slideLayout1.xml", &t);

    t = (Buf){0};
    buf_str(&t, "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster\" Target=\"../slideMasters/slideMaster1.xml\"/></Relationships>");
    zip_text(&zip, &e[7], L"ppt/slideLayouts/_rels/slideLayout1.xml.rels", &t);

    int k = 8;
    WCHAR name[128];
    for (int i = 1; i <= n; i++) {
        double scale = MINI(9144000.0 / pages[i - 1].w, 6858000.0 / pages[i - 1].h);
        int w = (int)(pages[i - 1].w * scale), h = (int)(pages[i - 1].h * scale);
        int x = (9144000 - w) / 2, y = (6858000 - h) / 2;
        t = (Buf){0};
        buf_fmt(&t, "<?xml version=\"1.0\" encoding=\"UTF-8\"?><p:sld xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"><p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/><p:pic><p:nvPicPr><p:cNvPr id=\"2\" name=\"image\"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr><p:blipFill><a:blip r:embed=\"rId2\"/><a:stretch><a:fillRect/></a:stretch></p:blipFill><p:spPr><a:xfrm><a:off x=\"%d\" y=\"%d\"/><a:ext cx=\"%d\" cy=\"%d\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic></p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sld>",
                x, y, w, h);
        wsprintfW(name, L"ppt/slides/slide%d.xml", i);
        zip_text(&zip, &e[k++], name, &t);

        t = (Buf){0};
        buf_str(&t, "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout\" Target=\"../slideLayouts/slideLayout1.xml\"/>");
        buf_fmt(&t, "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"../media/image%d.jpeg\"/>", i);
        buf_str(&t, "</Relationships>");
        wsprintfW(name, L"ppt/slides/_rels/slide%d.xml.rels", i);
        zip_text(&zip, &e[k++], name, &t);
    }
    for (int i = 1; i <= n; i++) {
        Buf jpg = {0};
        if (!jpeg_bytes(&pages[i - 1], quality, 100 + i, &jpg)) {
            mem_free(jpg.p);
            if (err)
                str_addf(err, T(STU_FAIL), path);
            goto fail;
        }
        wsprintfW(name, L"ppt/media/image%d.jpeg", i);
        zip_add(&zip, &e[k++], jpg.p, jpg.n, name);
        mem_free(jpg.p);
    }

    zip_finish(&zip, e, k);
    ok = export_write_file(path, zip.p, (int)zip.n);
fail:
    mem_free(zip.p);
    mem_free(e);
    if (!ok && err)
        str_addf(err, T(STU_FAIL), path);
    return ok;
}

bool export_images(const WCHAR *path, int fmt, int quality, ExportPage *pages, int count, Str *err)
{
    if (!path || !pages || count <= 0) {
        if (err)
            str_add(err, T(STU_NO_IMAGES));
        return false;
    }
    quality = CLAMPI(quality, 10, 100);
    if (fmt == 0)
        return write_pdf(path, pages, count, quality, err);
    if (fmt == 1)
        return write_pptx(path, pages, count, quality, err);
    return write_images(path, fmt, quality, pages, count, err);
}
