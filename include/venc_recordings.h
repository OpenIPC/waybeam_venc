/* venc_recordings.h — SD card file browser */
#pragma once

/**
 * Register all recordings routes with the HTTP server.
 * Call this from venc_webui_register() (see patch below).
 *
 * Routes added:
 *   GET /recordings                        → browser page
 *   GET /api/v1/recordings                 → JSON file list + free space
 *   GET /api/v1/recordings/download?file=  → file download
 *   GET /api/v1/recordings/delete?file=    → delete file (JSON)
 */
int venc_recordings_register(void);


/* ────────────────────────────────────────────────────────────────
 * HOW TO WIRE IN
 * ────────────────────────────────────────────────────────────────
 *
 * 1. Add venc_recordings.c to your Makefile:
 *
 *      SRCS += src/venc_recordings.c      # or wherever you put it
 *
 * 2. In venc_webui.c, add the include and one call:
 *
 *      #include "venc_recordings.h"       // ← add near top
 *
 *      int venc_webui_register(void)
 *      {
 *          int rc = 0;
 *          rc |= venc_recordings_register();   // ← BEFORE "/" catch-all
 *          rc |= venc_httpd_route("GET", "/", handle_dashboard, NULL);
 *          // !! ORDER MATTERS: "/" is a prefix match, register it LAST
 *          return rc;
 *      }
 *
 * 3. Build as usual:
 *
 *      make build SOC_BUILD=star6e
 *
 * 4. Deploy:
 *
 *      scp out/star6e/venc root@192.168.2.13:/usr/bin/venc
 *
 * 5. Open in browser:
 *
 *      http://192.168.2.13:8888/recordings
 *
 * ────────────────────────────────────────────────────────────────
 * ASSUMPTIONS about venc_httpd.h
 * ────────────────────────────────────────────────────────────────
 *
 * The code assumes these symbols already exist in your HTTP layer
 * (matching the patterns already used in venc_webui.c):
 *
 *   int venc_httpd_route(const char *method, const char *path,
 *                        int (*handler)(int fd, const HttpRequest*, void*),
 *                        void *ctx);
 *
 *   int httpd_send_html_gz(int fd, int status,
 *                          const unsigned char *data, int len);
 *
 *   int httpd_send_json(int fd, int status, const char *json);
 *
 *   const char *httpd_query_param(const HttpRequest *req,
 *                                 const char *key);
 *
 * If your actual function names differ, adjust the four call sites
 * in venc_recordings.c accordingly.
 *
 * ────────────────────────────────────────────────────────────────
 * SECURITY NOTES
 * ────────────────────────────────────────────────────────────────
 *
 * - safe_filename() rejects any name containing '/' or '\', so
 *   path traversal attacks (e.g. "../../etc/passwd") are blocked.
 * - Only .ts and .hevc files are listed; other files on the SD
 *   card are not exposed.
 * - The delete endpoint removes files without a confirmation step
 *   at the HTTP layer; the browser UI shows a confirm modal before
 *   calling it.
 */
