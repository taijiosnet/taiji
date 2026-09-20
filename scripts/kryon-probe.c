/* Kryon native Plan 9 probe, compiled and run in the guest by
 * scripts/kryon-smoke.sh.
 *
 * Exercises library surfaces that need no window: the SHA-256 and JSON
 * modules from kry_std and the compiled-in theme catalog from core. The
 * expected digest is the SHA-256 of "taiji-plan9-probe". Written for the
 * native environment: declarations at block start, no compound literals,
 * libc print/exits instead of hosted stdio. */

#include "kry_sha256.h"
#include "kry_json.h"
#include "theme.h"
#include "theme_meta.h"

static const char probe_input[] = "taiji-plan9-probe";
static const char probe_digest[] =
    "0d17e3db3e19121be8afce4434c9501b75fc72cab9c67fac75b05c1939980697";

static int
digest_matches(const unsigned char digest[32])
{
    int i;
    char byte[3];

    byte[2] = '\0';
    for(i = 0; i < 32; i++) {
        snprint(byte, sizeof(byte), "%02x", digest[i]);
        if(byte[0] != probe_digest[i * 2] || byte[1] != probe_digest[i * 2 + 1])
            return 0;
    }
    return 1;
}

void
main(void)
{
    KrySha256 sha;
    unsigned char digest[32];
    KryJson *doc;
    KryJson *flag;
    Theme theme;

    kry_sha256_init(&sha);
    kry_sha256_update(&sha, probe_input, strlen(probe_input));
    kry_sha256_final(&sha, digest);
    if(!digest_matches(digest)) {
        print("kryon-probe-failed: sha256 mismatch\n");
        exits("sha256");
    }

    doc = kry_json_parse("{\"probe\": true}");
    if(doc == nil) {
        print("kryon-probe-failed: json parse\n");
        exits("json");
    }
    flag = kry_json_get(doc, "probe");
    if(flag == nil || !kry_json_bool(flag)) {
        print("kryon-probe-failed: json access\n");
        kry_json_free(doc);
        exits("json");
    }
    kry_json_free(doc);

    theme = ThemeDefaultLight();
    if(theme.name == nil || theme.mode != THEME_MODE_LIGHT) {
        print("kryon-probe-failed: theme catalog\n");
        exits("theme");
    }

    print("kryon-probe-ok\n");
    exits(nil);
}
