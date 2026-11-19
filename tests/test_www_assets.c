#include <softline/www.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "asset line %d: %s\n", __LINE__, #c);                    \
      return 1;                                                                \
    }                                                                          \
  } while (0)
static char *asset(sl_www_t *w, const char *path) {
  sl_www_asset_t a;
  char *s;
  if (sl_www_asset(w, path, &a) != SL_OK)
    return NULL;
  s = malloc(a.length + 1);
  if (s) {
    memcpy(s, a.data, a.length);
    s[a.length] = 0;
  }
  return s;
}
int main(int argc, char **argv) {
  static const char *paths[] = {"/",
                                "/softline.css",
                                "/softline.js",
                                "/completion.js",
                                "/prompt.js",
                                "/transcripts.js",
                                "/images.js",
                                "/image-viewer.js",
                                "/markdown.js",
                                "/smd.js",
                                "/JetBrainsMono-Regular.woff2",
                                "/terminal",
                                "/terminal.js",
                                "/terminal-panel.js",
                                "/terminal.css",
                                "/xterm.js",
                                "/xterm-addon-fit.js",
                                "/xterm.css",
                                "/licenses/xterm.txt",
                                "/licenses/xterm-addon-fit.txt"};
  static const char *sources[] = {
      "web/index.html",
      "web/softline.css",
      "web/softline.js",
      "web/completion.js",
      "web/prompt.js",
      "web/transcripts.js",
      "web/images.js",
      "web/image-viewer.js",
      "web/markdown.js",
      "vendor/streaming-markdown/smd.js",
      "vendor/jetbrains-mono/JetBrainsMono-Regular.woff2",
      "web/terminal.html",
      "web/terminal.js",
      "web/terminal-panel.js",
      "web/terminal.css",
      "vendor/xterm/xterm.mjs",
      "vendor/xterm/addon-fit.mjs",
      "vendor/xterm/xterm.css",
      "vendor/xterm/LICENSE",
      "vendor/xterm/LICENSE.addon-fit"};
  sl_www_config_t cfg;
  sl_www_t *w;
  char *html, *css, *js, *md, *prompt, *at, *rule_end, *modules[4];
  size_t i;
  int depth = 0;
  CHECK(argc == 2);
  sl_www_config_init(&cfg);
  cfg.enabled = 1;
  w = sl_www_create(&cfg);
  CHECK(w);
  for (i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
    char file[4096];
    FILE *f;
    sl_www_asset_t a;
    size_t offset = 0;
    unsigned char bytes[4096];
    CHECK(sl_www_asset(w, paths[i], &a) == SL_OK);
    CHECK(snprintf(file, sizeof(file), "%s/%s", argv[1], sources[i]) <
          (int)sizeof(file));
    f = fopen(file, "rb");
    CHECK(f);
    for (;;) {
      size_t n = fread(bytes, 1, sizeof(bytes), f);
      CHECK(n <= a.length - offset && !memcmp(bytes, a.data + offset, n));
      offset += n;
      if (!n)
        break;
    }
    CHECK(!ferror(f) && offset == a.length);
    fclose(f);
  }
  html = asset(w, "/");
  css = asset(w, "/softline.css");
  js = asset(w, "/softline.js");
  md = asset(w, "/markdown.js");
  prompt = asset(w, "/prompt.js");
  modules[0] = asset(w, "/images.js");
  modules[1] = asset(w, "/image-viewer.js");
  modules[2] = asset(w, "/terminal-panel.js");
  modules[3] = asset(w, "/transcripts.js");
  CHECK(html && css && js && md && prompt && modules[0] && modules[1] &&
        modules[2] && modules[3]);
  CHECK(!strstr(html, "/terminal-lock") && !strstr(js, "/terminal-lock") &&
        !strstr(prompt, "/terminal-lock"));
  /* Logical DOM doubles cannot catch missing real controls. Verify the image
   * controllers' literal bindings against the actual embedded HTML as well. */
  for (i = 0; i < sizeof(modules) / sizeof(modules[0]); ++i) {
    const char *marker = "getElementById('";
    size_t prefix = strlen(marker);
    for (at = modules[i]; (at = strstr(at, marker)) != NULL;) {
      char id[128], *end;
      size_t n;
      at += prefix;
      end = strchr(at, '\'');
      CHECK(end && (n = (size_t)(end - at)) < 100);
      strcpy(id, "id=\"");
      memcpy(id + 4, at, n);
      id[n + 4] = '"';
      id[n + 5] = 0;
      CHECK(strstr(html, id));
    }
  }
  /* Every literal ID lookup in the shipped module names a real control. */
  for (at = js; (at = strstr(at, "$('")) != NULL; at += 3) {
    char id[128], *end = strchr(at + 3, '\'');
    size_t n;
    CHECK(end && (n = (size_t)(end - at - 3)) < 100);
    strcpy(id, "id=\"");
    memcpy(id + 4, at + 3, n);
    id[n + 4] = '"';
    id[n + 5] = 0;
    CHECK(strstr(html, id));
  }
  for (at = html; (at = strstr(at, "<option value=\"")) != NULL; ++at) {
    char selector[128], *end;
    size_t n;
    at += 15;
    end = strchr(at, '"');
    CHECK(end);
    n = (size_t)(end - at);
    CHECK(n < 60);
    if (!strncmp(at, "sans", n) || !strncmp(at, "mono", n) ||
        !strncmp(at, "default", n))
      continue;
    strcpy(selector, "data-theme=");
    memcpy(selector + 11, at, n);
    selector[11 + n] = ']';
    selector[12 + n] = 0;
    CHECK(strstr(css, selector));
  }
  for (at = css; *at; ++at) {
    if (*at == '{')
      ++depth;
    else if (*at == '}')
      --depth;
    CHECK(depth >= 0);
  }
  CHECK(!depth && strstr(css, "scrollbar-color") &&
        strstr(css, ".welcome-state #composer-area"));
  CHECK(
      strstr(html, "id=\"workspace\"") &&
      strstr(
          html,
          "id=\"terminal-panel\" aria-label=\"Server terminal panel\" hidden"));
  CHECK(strstr(css, "#workspace #chat{height:auto;flex:1;min-height:0}") &&
        strstr(css, "#terminal-panel{flex:none;") &&
        strstr(css, "@keyframes terminal-slide-up") &&
        strstr(css, "#composer[data-disabled=\"1\"]"));
  {
    char *page = asset(w, "/terminal"), *style = asset(w, "/terminal.css");
    char *grid, *footer;
    CHECK(page && style);
    grid = strstr(page, "<main id=\"terminal\"");
    footer = strstr(page, "<footer class=\"terminal-footer\"");
    CHECK(grid && footer && grid < footer && !strstr(page, "<header"));
    CHECK(strstr(footer, "id=\"terminal-status\"") &&
          strstr(footer, "id=\"terminal-close\""));
    at = strstr(style, ".terminal-footer{");
    CHECK(at && (rule_end = strchr(at, '}')) != NULL);
    CHECK(strstr(at, "height:24px;") && strstr(at, "height:24px;") < rule_end);
    CHECK(!strstr(style, "terminal-toolbar") &&
          strstr(style, "#terminal{flex:1;min-height:0;padding:8px 16px;"));
    free(page);
    free(style);
  }
  CHECK(strstr(html, "aria-autocomplete=\"inline\"") &&
        strstr(html, "aria-keyshortcuts=\"Escape\"") &&
        strstr(html, "id=\"prompt-projection\" aria-hidden=\"true\" hidden") &&
        strstr(css, "#completion-ghost{color:var(--muted)}") &&
        !strstr(js, "fileCompletion") && !strstr(js, "completionQuery"));
  /* Literal lookups in the prompt controller must also name shipped controls.
   */
  for (at = prompt; (at = strstr(at, "get('")) != NULL; at += 5) {
    char id[128], *end = strchr(at + 5, '\'');
    size_t n;
    CHECK(end && (n = (size_t)(end - at - 5)) < 100);
    strcpy(id, "id=\"");
    memcpy(id + 4, at + 5, n);
    id[n + 4] = '"';
    id[n + 5] = 0;
    CHECK(strstr(html, id));
  }
  CHECK(strstr(html, "id=\"activity\" role=\"status\"></div>"));
  CHECK(!strstr(js, "$('activity').hidden"));
  CHECK(!strstr(js, "body.innerText"));
  at = strstr(css, "#activity{");
  CHECK(at && (rule_end = strchr(at, '}')) != NULL);
  CHECK(strstr(at, "height:29px;") && strstr(at, "height:29px;") < rule_end);
  CHECK(strstr(at, "white-space:nowrap;") &&
        strstr(at, "white-space:nowrap;") < rule_end);
  CHECK(strstr(at, "overflow:hidden;") &&
        strstr(at, "overflow:hidden;") < rule_end);
  CHECK(strstr(js, "file.stream().getReader()") &&
        strstr(js, "navigator.clipboard.writeText") &&
        !strstr(js, "file.arrayBuffer()"));
  CHECK(strstr(md, "mdf-bar-chart") && strstr(md, "mdf-vertical-bar-chart") &&
        strstr(md, "mdf-tile-chart"));
  CHECK(!strstr(html, "http://") && !strstr(html, "https://"));
  CHECK(!strstr(js, "innerHTML") || strstr(js, "banner').innerHTML"));
  CHECK(!strstr(html, "microphone") && !strstr(html, "Read aloud") &&
        !strstr(html, "High"));
  free(html);
  free(css);
  free(js);
  free(md);
  free(prompt);
  for (i = 0; i < sizeof(modules) / sizeof(modules[0]); ++i)
    free(modules[i]);
  sl_www_destroy(w);
  return 0;
}
