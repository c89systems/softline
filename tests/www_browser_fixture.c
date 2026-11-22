#define _POSIX_C_SOURCE 200809L
#include "www_mock.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static volatile sig_atomic_t done;
static void stop(int signum) {
  (void)signum;
  done = 1;
}
int main(int argc, char **argv) {
  sl_www_config_t config;
  const char *animations = getenv("SOFTLINE_WEB_ANIMATIONS");
  sl_www_asset_t asset;
  demo_www_t *www;
  FILE *file;
  unsigned char *bytes;
  long length;
  int failed = 0;
  if (argc != 2) {
    fprintf(stderr, "usage: %s tests/web-browser.html\n", argv[0]);
    return 2;
  }
  file = fopen(argv[1], "rb");
  if (!file)
    return 1;
  if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 ||
      length > 65536 || fseek(file, 0, SEEK_SET)) {
    fclose(file);
    return 1;
  }
  bytes = malloc((size_t)length);
  if (!bytes || fread(bytes, 1, (size_t)length, file) != (size_t)length) {
    free(bytes);
    fclose(file);
    return 1;
  }
  fclose(file);
  sl_www_config_init(&config);
  config.enabled = 1;
  config.animations = animations && !strcmp(animations, "1");
  asset.path = "/browser-check.html";
  asset.content_type = "text/html; charset=utf-8";
  asset.data = bytes;
  asset.length = (size_t)length;
  config.assets = &asset;
  config.asset_count = 1;
  www = demo_www_open(&config, 8082, NULL);
  if (!www) {
    perror("browser check listener");
    free(bytes);
    return 1;
  }
  printf("Browser checks: http://127.0.0.1:%u/browser-check.html\n",
         (unsigned int)demo_www_port(www));
  fflush(stdout);
  signal(SIGINT, stop);
  signal(SIGTERM, stop);
  signal(SIGPIPE, SIG_IGN);
  while (!done)
    if (demo_www_poll(www, 100)) {
      failed = 1;
      break;
    }
  demo_www_close(www);
  free(bytes);
  return failed;
}
