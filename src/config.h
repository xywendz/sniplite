/* Settings (sniplite.ini next to the exe) and hotkey text <-> key codes. */
#pragma once
#include "common.h"

/* HK_OCR and HK_OCRCOPY are taken from the system only while the text recognition
 * module is installed. F5/F6/F7 drive recording, the shelf and the studio. */
enum {
    HK_CAPTURE = 1,
    HK_PIN,
    HK_TOGGLE,
    HK_OCR,
    HK_OCRCOPY,
    HK_RECORD,
    HK_SHELF,
    HK_STUDIO,
    HK_COUNT
};

typedef struct {
    UINT mod[HK_COUNT], vk[HK_COUNT]; /* index HK_*; vk 0 = no hotkey */
    bool gdi;                         /* GDI capture instead of DXGI */
    bool tray;                        /* show the notification-area icon */
    bool pin_top;                     /* every pin stays above other windows */
    bool auto_update;                 /* look for a newer release after start-up, then daily */
    int rec_fps;                      /* recording frame rate, 5..60 */
    int rec_bitrate;                  /* recording video bitrate in Mbps, 1..20 */
    bool shelf_on;                    /* show the hanger at the top of the screen */
    bool shelf_always;                /* expanded whenever the mouse is near, else F6 toggles */
    int shelf_theme;                  /* 0 light, 1 dark, 2 glass, 3 compact */
    bool shelf_add_capture;           /* every capture is also hung on the shelf */
    int export_fmt;                   /* 0 PDF, 1 PPTX, 2 PNG, 3 WEBP, 4 JPG */
    int export_quality;               /* 10..100 */
} Config;

void config_defaults(Config *c);             /* Snipaste-compatible: F1 / F3 / Shift+F3; for text F4 / Shift+F4 */
bool config_load(Config *c);                 /* defaults + sniplite.ini if present */
bool config_save(const Config *c);
void config_set_pin_top(bool on);            /* that one setting, changed from a pin's menu: into the file */
void config_path(WCHAR *out, int cap);

bool hotkey_parse(const WCHAR *s, UINT *mod, UINT *vk); /* "Ctrl+Alt+A", "F1", "none" */
void hotkey_format(UINT mod, UINT vk, WCHAR *out);      /* out: >= 40 chars; "none" if vk == 0 */
