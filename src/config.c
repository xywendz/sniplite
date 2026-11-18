/* Settings file and hotkey names.
 * sniplite.ini lives next to the exe and is only written from the preferences window;
 * without it everything uses the defaults. */
#include "config.h"
#include "feature.h"
#include "lang.h"

static const struct {
    const WCHAR *name;
    UINT vk;
} kKeys[] = {
    {L"PrtSc", VK_SNAPSHOT}, {L"PrintScreen", VK_SNAPSHOT}, {L"Print", VK_SNAPSHOT}, {L"Insert", VK_INSERT},
    {L"Ins", VK_INSERT},     {L"Delete", VK_DELETE},       {L"Del", VK_DELETE},     {L"Home", VK_HOME},
    {L"End", VK_END},        {L"PgUp", VK_PRIOR},          {L"PageUp", VK_PRIOR},   {L"PgDn", VK_NEXT},
    {L"PageDown", VK_NEXT},  {L"Pause", VK_PAUSE},         {L"ScrollLock", VK_SCROLL}, {L"Space", VK_SPACE},
    {L"Tab", VK_TAB},        {L"`", VK_OEM_3},             {L"Up", VK_UP},          {L"Down", VK_DOWN},
    {L"Left", VK_LEFT},      {L"Right", VK_RIGHT},
};

bool hotkey_parse(const WCHAR *s, UINT *mod, UINT *vk)
{
    *mod = *vk = 0;
    if (!*s || arg_eq(s, L"none") || arg_eq(s, L"off"))
        return true;
    WCHAR buf[64];
    lstrcpynW(buf, s, COUNTOF(buf));
    for (WCHAR *tok = buf, *next; tok; tok = next) {
        next = wcschr(tok, L'+');
        if (next)
            *next++ = 0;
        if (!*tok)
            return false;
        if (arg_eq(tok, L"ctrl") || arg_eq(tok, L"control"))
            *mod |= MOD_CONTROL;
        else if (arg_eq(tok, L"alt"))
            *mod |= MOD_ALT;
        else if (arg_eq(tok, L"shift"))
            *mod |= MOD_SHIFT;
        else if (arg_eq(tok, L"win"))
            *mod |= MOD_WIN;
        else if (*vk)
            return false; /* two keys */
        else if (!tok[1] && ((tok[0] >= L'a' && tok[0] <= L'z') || (tok[0] >= L'A' && tok[0] <= L'Z') ||
                             (tok[0] >= L'0' && tok[0] <= L'9')))
            *vk = (UINT)(tok[0] >= L'a' ? tok[0] - 32 : tok[0]);
        else if ((tok[0] == L'f' || tok[0] == L'F') && tok[1] >= L'1' && tok[1] <= L'9') {
            int n = 0;
            for (WCHAR *p = tok + 1; *p; p++) {
                if (*p < L'0' || *p > L'9')
                    return false;
                n = n * 10 + (*p - L'0');
            }
            if (n < 1 || n > 24)
                return false;
            *vk = VK_F1 + (UINT)n - 1;
        } else if ((tok[0] == L'n' || tok[0] == L'N') && !wcsncmp(tok + 1, L"um", 2) && tok[3] >= L'0' &&
                   tok[3] <= L'9' && !tok[4]) {
            *vk = VK_NUMPAD0 + (UINT)(tok[3] - L'0');
        } else {
            for (int i = 0; i < COUNTOF(kKeys) && !*vk; i++)
                if (arg_eq(tok, kKeys[i].name))
                    *vk = kKeys[i].vk;
            if (!*vk)
                return false;
        }
    }
    return *vk != 0;
}

void hotkey_format(UINT mod, UINT vk, WCHAR *out)
{
    out[0] = 0;
    if (!vk) {
        lstrcpyW(out, L"none");
        return;
    }
    if (mod & MOD_CONTROL) lstrcatW(out, L"Ctrl+");
    if (mod & MOD_ALT) lstrcatW(out, L"Alt+");
    if (mod & MOD_SHIFT) lstrcatW(out, L"Shift+");
    if (mod & MOD_WIN) lstrcatW(out, L"Win+");
    WCHAR k[16] = {0};
    if (vk >= VK_F1 && vk <= VK_F24)
        wsprintfW(k, L"F%u", vk - VK_F1 + 1);
    else if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        k[0] = (WCHAR)vk;
    else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        wsprintfW(k, L"Num%u", vk - VK_NUMPAD0);
    else
        for (int i = 0; i < COUNTOF(kKeys); i++)
            if (kKeys[i].vk == vk) {
                lstrcpyW(k, kKeys[i].name);
                break;
            }
    lstrcatW(out, k);
}

void config_defaults(Config *c)
{
    memset(c, 0, sizeof *c);
    hotkey_parse(L"F1", &c->mod[HK_CAPTURE], &c->vk[HK_CAPTURE]);
    hotkey_parse(L"F3", &c->mod[HK_PIN], &c->vk[HK_PIN]);
    hotkey_parse(L"Shift+F3", &c->mod[HK_TOGGLE], &c->vk[HK_TOGGLE]);
    hotkey_parse(L"F4", &c->mod[HK_OCR], &c->vk[HK_OCR]);
    hotkey_parse(L"Shift+F4", &c->mod[HK_OCRCOPY], &c->vk[HK_OCRCOPY]);
    hotkey_parse(L"F5", &c->mod[HK_RECORD], &c->vk[HK_RECORD]);
    hotkey_parse(L"F6", &c->mod[HK_SHELF], &c->vk[HK_SHELF]);
    hotkey_parse(L"F7", &c->mod[HK_STUDIO], &c->vk[HK_STUDIO]);
    c->tray = true;
    c->pin_top = true;
    c->auto_update = true;
    c->rec_fps = 30;
    c->rec_bitrate = 4;
    c->shelf_always = true;
    c->shelf_theme = 0;
    c->shelf_add_capture = true;
    c->export_fmt = 0;
    c->export_quality = 85;
}

void config_path(WCHAR *out, int cap) { ini_path(out, cap); }

static const WCHAR *kHkKeys[HK_COUNT] = {NULL, L"hotkey", L"pin_hotkey", L"toggle_hotkey", L"ocr_hotkey",
                                         L"ocr_copy_hotkey", L"record_hotkey", L"shelf_hotkey", L"studio_hotkey"};

bool config_load(Config *c)
{
    config_defaults(c);
    WCHAR path[MAX_PATH];
    config_path(path, MAX_PATH);
    if (!path[0] || GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        return false;
    WCHAR v[64];
    for (int id = 1; id < HK_COUNT; id++) {
        UINT mod, vk;
        GetPrivateProfileStringW(L"sniplite", kHkKeys[id], L"\x1F", v, COUNTOF(v), path);
        if (v[0] != 0x1F && hotkey_parse(v, &mod, &vk)) { /* a bad value keeps the default */
            c->mod[id] = mod;
            c->vk[id] = vk;
        }
    }
    GetPrivateProfileStringW(L"sniplite", L"capture", L"dxgi", v, COUNTOF(v), path);
    c->gdi = arg_eq(v, L"gdi");
    c->tray = GetPrivateProfileIntW(L"sniplite", L"tray", 1, path) != 0;
    c->pin_top = GetPrivateProfileIntW(L"sniplite", L"pin_top", 1, path) != 0;
    c->auto_update = GetPrivateProfileIntW(L"sniplite", L"auto_update", 1, path) != 0;
    c->rec_fps = CLAMPI((int)GetPrivateProfileIntW(L"sniplite", L"rec_fps", 30, path), 5, 60);
    c->rec_bitrate = CLAMPI((int)GetPrivateProfileIntW(L"sniplite", L"rec_bitrate", 4, path), 1, 20);
    c->shelf_on = GetPrivateProfileIntW(L"sniplite", L"shelf", 0, path) != 0;
    c->shelf_always = GetPrivateProfileIntW(L"sniplite", L"shelf_always", 1, path) != 0;
    c->shelf_theme = CLAMPI((int)GetPrivateProfileIntW(L"sniplite", L"shelf_theme", 0, path), 0, SH_THEME_COUNT - 1);
    c->shelf_add_capture = GetPrivateProfileIntW(L"sniplite", L"shelf_capture", 1, path) != 0;
    c->export_fmt = CLAMPI((int)GetPrivateProfileIntW(L"sniplite", L"export_fmt", 0, path), 0, 4);
    c->export_quality = CLAMPI((int)GetPrivateProfileIntW(L"sniplite", L"export_quality", 85, path), 10, 100);
    return true;
}

bool config_save(const Config *c)
{
    WCHAR path[MAX_PATH];
    config_path(path, MAX_PATH);
    if (!path[0])
        return false;
    Str s = {0};
    str_add(&s, T(INI_1));
    str_add(&s, T(INI_2));
    str_add(&s, T(INI_3));
    str_add(&s, L"[sniplite]\r\n");
    for (int id = 1; id < HK_COUNT; id++) {
        WCHAR k[40];
        hotkey_format(c->mod[id], c->vk[id], k);
        str_addf(&s, L"%s=%s\r\n", kHkKeys[id], k);
    }
    str_addf(&s, L"capture=%s\r\ntray=%d\r\nauto_update=%d\r\npin_top=%d\r\n", c->gdi ? L"gdi" : L"dxgi", c->tray ? 1 : 0,
             c->auto_update ? 1 : 0, c->pin_top ? 1 : 0);
    str_addf(&s, L"rec_fps=%d\r\nrec_bitrate=%d\r\n", c->rec_fps, c->rec_bitrate);
    str_addf(&s, L"shelf=%d\r\nshelf_always=%d\r\nshelf_theme=%d\r\nshelf_capture=%d\r\n", c->shelf_on ? 1 : 0,
             c->shelf_always ? 1 : 0, c->shelf_theme, c->shelf_add_capture ? 1 : 0);
    str_addf(&s, L"export_fmt=%d\r\nexport_quality=%d\r\n", c->export_fmt, c->export_quality);
    if (lang_choice() != LG_AUTO) /* kept by lang.c; without the line the language follows Windows */
        str_addf(&s, L"lang=%s\r\n", lang_code(lang_choice()));
    /* UTF-8 without BOM: Notepad shows the comments, and the ASCII keys/values
     * read back fine through GetPrivateProfileString. */
    int n = WideCharToMultiByte(CP_UTF8, 0, s.p, s.n, NULL, 0, NULL, NULL);
    char *b = n > 0 ? mem_alloc((size_t)n) : NULL;
    bool ok = false;
    if (b) {
        WideCharToMultiByte(CP_UTF8, 0, s.p, s.n, b, n, NULL, NULL);
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD w = 0;
        if (f != INVALID_HANDLE_VALUE) {
            ok = WriteFile(f, b, (DWORD)n, &w, NULL) && w == (DWORD)n;
            CloseHandle(f);
        }
        mem_free(b);
    }
    str_free(&s);
    return ok;
}

void config_set_pin_top(bool on)
{
    Config c;
    config_load(&c); /* what the file says, not what a command line changed for this run */
    c.pin_top = on;
    config_save(&c);
}
