#ifndef EVA_OTA_H
#define EVA_OTA_H

#include <stdbool.h>
#include <stddef.h>

#include "eva_ota_status.h"

/* Wi-Fi OTA: an HTTP server the Mac pushes builds to, so the USB cable is
 * never needed again after the one-time partition-table flash.
 *
 * Push (device runs a server) rather than poll (device fetches a URL) because
 * the Mac then needs no long-running server of its own, the update starts the
 * moment the script runs, and tools/ota.py sees real HTTP status codes instead
 * of having to infer failures from device logs.
 *
 * Endpoints (port 8080, plain HTTP — LAN only, no auth by design):
 *   GET  /ota/status  what the device currently has (hashes, version, slot)
 *   GET  /ota/ping    liveness + version, polled after a reboot
 *   POST /ota/app     app image; body is raw bytes, X-Eva-Sha256 header
 *   POST /ota/pack    clouds.bin; same shape
 *   POST /ota/reboot  reboot without pushing anything
 */

/* Starts the OTA task, which waits for Wi-Fi, brings up mDNS
 * (eva-weather.local) and starts the HTTP server. Safe to call once from
 * app_main. `status` may be NULL (no on-screen banner). */
void eva_ota_start(eva_ota_status_t *status);

/* Start/stop just the HTTP server, for the CDC `otaserver on|off` command —
 * used to isolate whether an idle httpd costs any FPS. */
bool eva_ota_server_start(void);
void eva_ota_server_stop(void);
bool eva_ota_server_running(void);

/* Human-readable state for the CDC `otainfo` command. */
void eva_ota_info(char *buf, size_t buf_len);

/* Build version string, also reported over HTTP. */
const char *eva_ota_version(void);

#endif
