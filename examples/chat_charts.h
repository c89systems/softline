#ifndef SOFTLINE_CHAT_CHARTS_H
#define SOFTLINE_CHAT_CHARTS_H
#include <stdio.h>
#include <string.h>

#define DEMO_CHART_CAPACITY 512
typedef struct demo_charts {
  unsigned long random;
  unsigned int turns, order[3], next;
} demo_charts_t;

static void demo_charts_init(demo_charts_t *charts, unsigned long seed) {
  memset(charts, 0, sizeof(*charts));
  charts->random = seed & 0xffffffffUL;
  charts->next = 3;
}
static unsigned long demo_chart_random(demo_charts_t *charts) {
  charts->random = (charts->random * 1664525UL + 1013904223UL) & 0xffffffffUL;
  return charts->random;
}
/* A separate bounded source segment: callers stream it after the ordinary
 * reply instead of collecting a response in a concatenation buffer. */
static int demo_chart_next(demo_charts_t *charts, char *output, size_t size) {
  static const char *const kinds[] = {"mdf-bar-chart", "mdf-vertical-bar-chart",
                                      "mdf-tile-chart"};
  static const char *const labels[][3] = {{"Planning", "Building", "Review"},
                                          {"Parsing", "Rendering", "Testing"},
                                          {"Queued", "Running", "Complete"}};
  const char *kind;
  unsigned int i, label;
  unsigned long values[3];
  int length;
  if (!size)
    return -1;
  output[0] = 0;
  if (++charts->turns < 3)
    return 0;
  charts->turns = 0;
  if (charts->next == 3) {
    for (i = 0; i < 3; ++i)
      charts->order[i] = i;
    for (i = 2; i > 0; --i) {
      unsigned int j = (unsigned int)(demo_chart_random(charts) % (i + 1));
      unsigned int swap = charts->order[i];
      charts->order[i] = charts->order[j];
      charts->order[j] = swap;
    }
    charts->next = 0;
  }
  kind = kinds[charts->order[charts->next++]];
  if (!strcmp(kind, "mdf-bar-chart") && demo_chart_random(charts) % 2)
    kind = "mdf-horizontal-bar-chart";
  label = (unsigned int)(demo_chart_random(charts) % 3);
  for (i = 0; i < 3; ++i)
    values[i] = 1 + demo_chart_random(charts) % 100;
  length = snprintf(output, size,
                    "\n## Progress snapshot\n\n"
                    "```%s,sort=%s,colored-bars=on\n"
                    "Stage,Value\n%s,%lu\n%s,%lu\n%s,%lu\n```\n",
                    kind, demo_chart_random(charts) % 2 ? "asc" : "desc",
                    labels[label][0], values[0], labels[label][1], values[1],
                    labels[label][2], values[2]);
  return length < 0 || (size_t)length >= size ? -1 : 1;
}
#endif
