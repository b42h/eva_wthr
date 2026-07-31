#ifndef EVA_OTA_STATE_H
#define EVA_OTA_STATE_H

#include <stdbool.h>

/* Records WHICH artifacts the device currently has, so tools/ota.py can skip
 * transferring anything that has not changed.
 *
 * The stored hash is the one the HOST told us it sent — never a hash the device
 * recomputes from flash. Reproducing a file hash from a flashed partition is
 * subtly different (image headers, appended checksums, padding), and getting it
 * wrong would silently re-transfer 7.6 MB on every run. Recording the host's
 * value costs one wasted transfer on first run and is exact forever after.
 *
 * Free functions rather than a handle: these are called from the httpd task,
 * from app_main, and from the cloud-assets init path, none of which share an
 * object. NVS namespace "eva" is shared with eva_settings.c. */

#define EVA_OTA_SHA_HEX_LEN 64   /* SHA-256 as lowercase hex, without NUL */

/* Copy the recorded hash into `out` (must hold EVA_OTA_SHA_HEX_LEN + 1 bytes).
 * Writes "" and returns false when nothing is recorded yet. */
bool eva_ota_state_get_app_sha(char *out);
bool eva_ota_state_get_pack_sha(char *out);

/* Record the hash the host sent. Call only AFTER the artifact is committed
 * (esp_ota_set_boot_partition succeeded / the pack verified). */
void eva_ota_state_set_app_sha(const char *hex);
void eva_ota_state_set_pack_sha(const char *hex);

/* Forget a hash so the next ota.py run re-sends that artifact. Used when the
 * pack fails to parse at boot, and when a rollback is detected. */
void eva_ota_state_clear_app_sha(void);
void eva_ota_state_clear_pack_sha(void);

/* Label of the partition the app was written to, recorded at commit time.
 * Compared against the running partition at boot: a mismatch means the
 * bootloader rolled back, so the recorded app hash no longer describes what is
 * running and must be cleared. */
bool eva_ota_state_get_app_part(char *out, int cap);
void eva_ota_state_set_app_part(const char *label);

/* Compare the running partition against the recorded label and clear the app
 * hash if they differ. Call once at boot, after NVS init. Returns true when a
 * rollback was detected. */
bool eva_ota_state_check_rollback(void);

#endif
