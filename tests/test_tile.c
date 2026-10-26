/* Developed by X-F1REBALL-X. Host test for the home-screen tile files. */
#define _DEFAULT_SOURCE
#include "tile.h"
#include "icon_png.h"
#include "pic_png.h"
#include "start_html.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(c, m) do { if (c) printf("ok   %s\n", m); else { printf("FAIL %s\n", m); fails++; } } while (0)

static long fsize(const char *p) { struct stat st; return stat(p, &st) ? -1 : (long)st.st_size; }

int main(void)
{
    char root[] = "/tmp/hb_tile_XXXXXX", p[512], js[2048], small[16];
    int n;

    CHECK(sizeof hb_icon_png > 8 && !memcmp(hb_icon_png, "\x89PNG\r\n\x1a\n", 8), "embedded icon is a PNG");
    CHECK(hb_icon_png[16] == 0 && hb_icon_png[18] == 2 && hb_icon_png[19] == 0 &&
          hb_icon_png[20] == 0 && hb_icon_png[22] == 2 && hb_icon_png[23] == 0, "icon is 512x512");
    CHECK(sizeof hb_pic_png > 8 && !memcmp(hb_pic_png, "\x89PNG\r\n\x1a\n", 8), "embedded background is a PNG");
    CHECK(hb_pic_png[16] == 0 && hb_pic_png[17] == 0 && hb_pic_png[18] == 0x07 && hb_pic_png[19] == 0x80 &&
          hb_pic_png[20] == 0 && hb_pic_png[21] == 0 && hb_pic_png[22] == 0x04 && hb_pic_png[23] == 0x38,
          "background is 1920x1080");
    CHECK(sizeof hb_pic_png < 2u * 1024 * 1024, "background stays under 2 MB (it is written twice at every start)");
    n = tile_manifest(js, sizeof js, NULL);
    CHECK(n > 0 && strstr(js, "\"titleId\": \"" HB_TILE_ID "\"") &&
          strstr(js, "\"deeplinkUri\": \"" HB_TILE_URL "\"") &&
          strstr(js, "\"applicationCategoryType\": 65536,") && strstr(js, "\"contentBadgeType\": 1,"),
          "manifest fields (tile under Media)");
    CHECK(!strstr(js, "\"applicationCategoryType\": 0,"), "manifest: no longer a Games app");
    CHECK(tile_version_mismatch(NULL) && tile_version_mismatch("1\n") &&
          tile_version_mismatch("2\n") && tile_version_mismatch("3\n"),
          "tile version: missing or older -> mismatch");
    CHECK(!tile_version_mismatch(HB_TILE_VERSION "\n") && !tile_version_mismatch(HB_TILE_VERSION "\r\n"),
          "tile version: current -> no mismatch");
    CHECK(!tile_needs_reinstall(NULL, 0) && !tile_needs_reinstall("1", 0), "tile version: nothing installed, needs_reinstall false");
    CHECK(tile_needs_reinstall(NULL, 1), "tile version: installed by 1.1.0 (no version file) -> reinstall once");
    CHECK(tile_needs_reinstall("1\n", 1), "tile version: older version -> reinstall");
    CHECK(!tile_needs_reinstall(HB_TILE_VERSION "\n", 1) && !tile_needs_reinstall(HB_TILE_VERSION "\r\n", 1),
          "tile version: current version -> left alone");
    CHECK(strstr(js, "\"defaultLanguage\": \"en-US\"") && strstr(js, "\"he-IL\""), "manifest languages");
    CHECK(tile_manifest(small, sizeof small, NULL) == -1, "manifest refuses a short buffer");
    {
        char j2[2048];
        CHECK(tile_manifest(j2, sizeof j2, HB_TILE_START_URL) > 0 &&
              strstr(j2, "\"deeplinkUri\": \"http://127.0.0.1:8090/bqd\""),
              "manifest takes the /bqd link");
        CHECK(!strstr(j2, "file://") && atoi(HB_TILE_VERSION) > 8, "no file:// link, tile version bumped past 8");
    }
    CHECK(strstr((const char *)hb_start_html, "127.0.0.1:8090") &&
          strstr((const char *)hb_start_html, "BossQuickDock is not running") &&
          strstr((const char *)hb_start_html, "ar:[") && !strstr((const char *)hb_start_html, "he:["),
          "fallback page probes the server, has languages, no Hebrew");
    CHECK(strstr((const char *)hb_start_html, "<html lang=\"en\" manifest=\"/bqd.appcache\">"),
          "start page is an AppCache master entry");
    CHECK(strstr((const char *)hb_start_html, "ping?m=xhr") && strstr((const char *)hb_start_html, "ping?m=fetch") &&
          strstr((const char *)hb_start_html, "ping.gif") && strstr((const char *)hb_start_html, "ping.js") &&
          strstr((const char *)hb_start_html, "ping.html") && !strstr((const char *)hb_start_html, "navigator.onLine===true"),
          "start page probes by XHR, fetch, image, script and frame");
    CHECK(strstr(js, "\"titleName\": \"BossQuickDock\"") && !strstr(js, "PS5"), "tile title is plain BossQuickDock");
    CHECK(!strstr((const char *)hb_start_html, "PS5.elf") && !strstr((const char *)hb_start_html, "PS5") && !strstr((const char *)hb_start_html, "PS 5") &&
          strstr((const char *)hb_start_html, "Load BossQuickDock with your ELF loader first"),
          "start page does not name the ELF file");
    CHECK(!strstr((const char *)hb_start_html, "ailbreak") && strstr((const char *)hb_start_html, "Checking...") &&
          strstr((const char *)hb_start_html, "/debug/.test(location.search+location.hash)"),
          "start page: no jailbreak step, Checking state, debug line only with #debug");
    CHECK(strstr((const char *)hb_start_html, "class=\"f w\" id=\"f\"") && strstr((const char *)hb_start_html, "E('f').style.display='block'"),
          "start page: footer only after the probes conclude");
    CHECK(strstr((const char *)hb_start_html, "id=\"o\" href=\"http://127.0.0.1:8090/\""),
          "start page has an Open button that goes straight to the control page");

    if (!mkdtemp(root)) return 1;
    CHECK(tile_files_put(root, NULL, hb_icon_png, sizeof hb_icon_png, hb_pic_png, sizeof hb_pic_png,
                         hb_start_html, HB_START_HTML_LEN) == 1, "first put writes");
    snprintf(p, sizeof p, "%s/%s/sce_sys/icon0.png", root, HB_TILE_ID);
    CHECK(fsize(p) == (long)sizeof hb_icon_png, "icon0.png written");
    snprintf(p, sizeof p, "%s/%s/sce_sys/pic0.png", root, HB_TILE_ID);
    CHECK(fsize(p) == (long)sizeof hb_pic_png, "pic0.png (home screen background) written");
    snprintf(p, sizeof p, "%s/%s/sce_sys/pic1.png", root, HB_TILE_ID);
    CHECK(fsize(p) == (long)sizeof hb_pic_png, "pic1.png (launch background) written");
    snprintf(p, sizeof p, "%s/%s/sce_sys/param.json", root, HB_TILE_ID);
    CHECK(fsize(p) == n, "param.json written");
    snprintf(p, sizeof p, "%s/%s/start.html", root, HB_TILE_ID);
    CHECK(fsize(p) == (long)HB_START_HTML_LEN, "start.html written");
    CHECK(tile_files_put(root, NULL, hb_icon_png, sizeof hb_icon_png, hb_pic_png, sizeof hb_pic_png,
                         hb_start_html, HB_START_HTML_LEN) == 0, "second put is a no-op");
    CHECK(tile_files_put(root, "http://127.0.0.1:8090/", hb_icon_png, sizeof hb_icon_png,
                         hb_pic_png, sizeof hb_pic_png, NULL, 0) == 1, "changed link is rewritten");
    CHECK(tile_files_put(root, NULL, (const unsigned char *)"x", 1, hb_pic_png, sizeof hb_pic_png, NULL, 0) == 1,
          "changed icon is rewritten");
    CHECK(tile_files_put(root, NULL, (const unsigned char *)"x", 1, (const unsigned char *)"yy", 2, NULL, 0) == 1,
          "changed background is rewritten");
    snprintf(p, sizeof p, "%s/%s/sce_sys/pic0.png", root, HB_TILE_ID);
    CHECK(fsize(p) == 2, "pic0.png holds the new background");
    CHECK(tile_files_put(root, NULL, (const unsigned char *)"x", 1, NULL, 0, NULL, 0) == 0,
          "put without a background leaves the files alone");
    CHECK(fsize(p) == 2, "put without a background keeps pic0.png");
    CHECK(tile_files_write(root, NULL, (const unsigned char *)"x", 1, NULL, 0, NULL, 0) == 0 && fsize(p) == -1,
          "write without a background removes it");
    CHECK(tile_files_write(root, NULL, (const unsigned char *)"x", 1, hb_pic_png, sizeof hb_pic_png, NULL, 0) == 0 &&
          fsize(p) == (long)sizeof hb_pic_png, "write puts the background back");
    tile_files_drop(root);
    snprintf(p, sizeof p, "%s/%s", root, HB_TILE_ID);
    CHECK(fsize(p) == -1, "drop removes the folder (background included)");
    CHECK(tile_files_put("/nonexistent/dir", NULL, hb_icon_png, sizeof hb_icon_png, hb_pic_png, sizeof hb_pic_png,
                         NULL, 0) == -1, "bad root fails");
    rmdir(root);
    printf(fails ? "FAILED (%d)\n" : "ALL OK (0 failures)\n", fails);
    return fails != 0;
}
