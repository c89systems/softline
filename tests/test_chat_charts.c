#include "../examples/chat_charts.h"
#include <libmdf/mdf.h>
#include <stdlib.h>

#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "chart line %d: %s\n", __LINE__, #c);                    \
      return 1;                                                                \
    }                                                                          \
  } while (0)
struct output {
  char *bytes;
  size_t length;
};
static int write_output(void *data, const char *bytes, size_t length) {
  struct output *out = data;
  if (length >= 32768 - out->length)
    return -1;
  memcpy(out->bytes + out->length, bytes, length);
  out->length += length;
  out->bytes[out->length] = 0;
  return 0;
}
static int render_chart(const char *chart) {
  mdf_options options;
  mdf_sink sink;
  mdf *renderer;
  struct output out;
  size_t i;
  out.bytes = malloc(32768);
  out.length = 0;
  CHECK(out.bytes);
  out.bytes[0] = 0;
  mdf_options_init(&options);
  options.ansi_mode = MDF_ANSI_OFF;
  options.boring = 1;
  options.width = 72;
  CHECK(mdf_create(MDF_FORMAT_ANSI, &options, &renderer) == MDF_OK);
  sink.write = write_output;
  sink.userdata = &out;
  CHECK(renderer->set_sink(renderer, &sink) == MDF_OK);
  for (i = 0; chart[i]; ++i)
    CHECK(renderer->feed(renderer, chart + i, 1) == MDF_OK);
  CHECK(renderer->finish_document(renderer) == MDF_OK);
  CHECK(strstr(out.bytes, "Progress snapshot") && !strstr(out.bytes, "mdf-") &&
        !strstr(out.bytes, "Stage,Value"));
  renderer->destroy(renderer);
  free(out.bytes);
  return 0;
}
int main(void) {
  demo_charts_t charts, replay;
  char chart[DEMO_CHART_CAPACITY], same[DEMO_CHART_CAPACITY];
  unsigned long seed;
  int expected, turn, varied = 0;
  unsigned long first_values[3] = {0, 0, 0};
  srand(1234);
  expected = rand();
  srand(1234);
  for (seed = 0; seed < 24; ++seed) {
    unsigned int mask = 0;
    demo_charts_init(&charts, seed);
    demo_charts_init(&replay, seed);
    for (turn = 1; turn <= 90; ++turn) {
      int result = demo_chart_next(&charts, chart, sizeof(chart));
      CHECK(result == demo_chart_next(&replay, same, sizeof(same)) &&
            !strcmp(chart, same));
      CHECK(result == (turn % 3 == 0));
      if (result) {
        unsigned long values[3];
        const char *rows = strstr(chart, "Stage,Value\n");
        CHECK(strstr(chart, "Stage,Value\n") &&
              strstr(chart, "colored-bars=on"));
        CHECK(rows && sscanf(rows + strlen("Stage,Value\n"),
                             "%*[^,],%lu\n%*[^,],%lu\n%*[^,],%lu", values,
                             values + 1, values + 2) == 3);
        CHECK(values[0] >= 1 && values[0] <= 100 && values[1] >= 1 &&
              values[1] <= 100 && values[2] >= 1 && values[2] <= 100);
        if (turn == 3) {
          if (!seed)
            memcpy(first_values, values, sizeof(values));
          else if (memcmp(first_values, values, sizeof(values)))
            varied = 1;
        }
        if (strstr(chart, "mdf-vertical-bar-chart"))
          mask |= 2;
        else if (strstr(chart, "mdf-tile-chart"))
          mask |= 4;
        else {
          CHECK(strstr(chart, "mdf-bar-chart") ||
                strstr(chart, "mdf-horizontal-bar-chart"));
          mask |= 1;
        }
        if (seed < 3 && turn <= 9)
          CHECK(!render_chart(chart));
      } else
        CHECK(!chart[0]);
      if (turn % 9 == 0) {
        CHECK(mask == 7);
        mask = 0;
      }
    }
  }
  CHECK(rand() == expected);
  CHECK(varied);
  demo_charts_init(&charts, 1);
  CHECK(demo_chart_next(&charts, chart, 1) == 0);
  CHECK(demo_chart_next(&charts, chart, 1) == 0);
  CHECK(demo_chart_next(&charts, chart, 1) == -1);
  return 0;
}
