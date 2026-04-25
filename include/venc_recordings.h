#ifndef VENC_RECORDINGS_H
#define VENC_RECORDINGS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Register the recordings JSON API routes with the httpd:
 *   GET /api/v1/recordings                  list .ts/.hevc files
 *   GET /api/v1/recordings/download?file=   stream file as download
 *   GET /api/v1/recordings/delete?file=     remove a file
 *
 * The browser UI for these endpoints lives in the Recordings tab on the
 * dashboard served by venc_webui.c — no separate HTML route is registered.
 *
 * Returns 0 on success, -1 if any route registration failed. */
int venc_recordings_register(void);

#ifdef __cplusplus
}
#endif

#endif /* VENC_RECORDINGS_H */
