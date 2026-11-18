/* Update check and download (GitHub releases). Only ever runs in short-lived
 * helper processes, never in the background process. */
#pragma once
#include "common.h"

#ifndef UPDATE_REPO /* a test build may point elsewhere */
#define UPDATE_REPO L"/xywendz/sniplite"
#endif
#define UPDATE_HOST L"github.com"
/* The installer: as build.bat makes it, and under this very name in every release.
 * Installed copies ask for it by name when they update, so it must not be renamed. */
#define SETUP_FILE L"sniplite_win_setup_amd64.exe"

typedef enum {
    UPD_ERROR,  /* no network, GitHub unreachable, ... */
    UPD_NONE,   /* no release published yet */
    UPD_LATEST, /* this is the newest version */
    UPD_NEWER,  /* tag holds a newer release */
} UpdState;

typedef struct {
    UpdState state;
    WCHAR tag[32]; /* e.g. "v1.0.1" */
} UpdateResult;

int version_cmp(const WCHAR *a, const WCHAR *b); /* "1.0.0" vs "v1.0.1": <0, 0, >0 */

/* Blocking (a few seconds at most); call from a worker thread. */
void update_check(UpdateResult *r);

/* Download the installer of `tag` to %TEMP%. progress(percent, ctx) may be called
 * repeatedly with 0..100, or with -1 when the size is unknown. Blocking. */
bool update_download(const WCHAR *tag, WCHAR *path, int cap, void (*progress)(int, void *), void *ctx);

/* GET https://host/path, following redirects; the body goes to sink piece by piece
 * (false from it ends the transfer). headers: more request headers, each ending in
 * CR LF, or NULL. *status is set before the first piece: 200, or 206 for a range.
 * True when the whole body went through. Blocking. */
bool http_fetch(const WCHAR *host, const WCHAR *path, const WCHAR *headers, DWORD *status,
                bool (*sink)(const void *data, DWORD n, void *ctx), void *ctx);

/* An installed copy has sniplite.com beside sniplite.exe, in whatever folder was chosen:
 * the installer always writes both. The portable program is sniplite.exe by itself. */
bool update_installed_copy(void);
/* The command that has the downloaded installer replace this copy, where it is:
 * "<setup>" /update "/D=<this program's folder>" */
void update_setup_cmd(Str *cmd, const WCHAR *setup);

/* `sniplite.exe --update-check`: if a newer release exists, tell the background process. */
int update_check_main(void);
