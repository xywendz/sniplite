/* sniplite - optional creative tools: video recording, the shelf and the studio.
 * These modules are ordinary C; the capture hot path stays in assembly. */
#pragma once
#include "common.h"

/* ---- video recording (rec.c) -------------------------------------------------- */
bool rec_init(HINSTANCE inst, HWND daemon);
void rec_shutdown(void);
bool rec_active(void);
/* Start a recording. path NULL: choose a file in the Pictures folder. hwnd NULL
 * means the whole virtual screen; otherwise the window's visible rectangle. */
bool rec_start(bool fullscreen, HWND hwnd, const WCHAR *path, int fps, int bitrate, Str *err);
void rec_stop(void);
/* Interactive picker: click a window to record it, Enter for the full screen. */
void rec_pick(void);
int  rec_cmd(int argc, WCHAR **argv, const WCHAR *cwd, bool json, Str *out);
void rec_indicator_tick(void);

/* ---- shelf (shelf.c) ----------------------------------------------------------- */
#define SH_THEME_COUNT 4
bool shelf_init(HINSTANCE inst, HWND daemon);
void shelf_shutdown(void);
bool shelf_add_image(Img *im);           /* takes its own copy */
bool shelf_add_text(const WCHAR *text, int len);
bool shelf_add_file(const WCHAR *path);  /* one file */
bool shelf_add_files(const WCHAR *list); /* double-NUL terminated, from CF_HDROP */
bool shelf_add_clipboard(HWND hwnd);
bool shelf_add_capture(Img *im);         /* obeys the "add captures" setting */
void shelf_show(bool show);
void shelf_toggle(void);
bool shelf_visible(void);
void shelf_apply_config(void);
void shelf_set_theme(int theme);
int  shelf_count(void);
int  shelf_cmd(int argc, WCHAR **argv, bool json, Str *out);

/* ---- studio (studio.c) --------------------------------------------------------- */
enum { STUDIO_PUZZLE = 0, STUDIO_MERGE = 1 };
bool studio_init(HINSTANCE inst, HWND daemon);
void studio_shutdown(void);
void studio_open(int mode);
int  studio_cmd(int argc, WCHAR **argv, const WCHAR *cwd, bool json, Str *out);

/* ---- exporters (exporter.c) ---------------------------------------------------- */
/* A page for the document merger and the collage: 32bpp top-down pixels. */
typedef struct {
    uint32_t *px;
    int w, h;
} ExportPage;
bool export_images(const WCHAR *path, int fmt, int quality, ExportPage *pages, int count, Str *err);
/* fmt: 0 PDF, 1 PPTX, 2 PNG, 3 WEBP, 4 JPG. PNG/WEBP/JPG write page 0 only
 * unless path ends in a directory (then one numbered file per page). */
bool export_write_file(const WCHAR *path, const void *data, int len);
