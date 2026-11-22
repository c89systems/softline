#ifndef SOFTLINE_TEST_VTE_OUTPUT_SYNC_H
#define SOFTLINE_TEST_VTE_OUTPUT_SYNC_H
#include "vte_test.h"

/* A title update is ordered with the test input and leaves the cells and
 * cursor alone. Use the slave fd when earlier output came through the PTY. */
static int output_parsed(struct screen *s, int slave) {
  static unsigned long sequence;
  char title[40], marker[48];
  int length;
  double deadline = now() + 2;
  snprintf(title, sizeof(title), "softline-test-%lu", ++sequence);
  length = snprintf(marker, sizeof(marker), "\033]2;%s\007", title);
  if (slave < 0)
    v.feed(s->terminal, marker, length);
  else if (write(slave, marker, (size_t)length) != length)
    return 1;
  do {
    const char *seen = v.title(s->terminal);
    if (seen && !strcmp(seen, title))
      return 0;
    pump_once();
  } while (now() < deadline);
  fprintf(stderr, "VTE did not parse the output marker\n");
  return 1;
}
static int output_closed(struct screen *s, double deadline) {
  /* VTE detaches the PTY after parsing the bytes up to EOF. */
  while (v.get_pty(s->terminal)) {
    if (now() >= deadline) {
      fprintf(stderr, "VTE did not drain the closed PTY\n");
      return 1;
    }
    pump_once();
  }
  return 0;
}
#endif
