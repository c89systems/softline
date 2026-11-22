#include "tmux_test.h"

#define RESIZE 1
#define HEIGHT 2
#define STATIC 4
#define ROUNDTRIP 8
#define LIVE 16
#define PAGED 32
#define PREFILLED 64
#define GROW 128
#define QUOTED 256
#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      struct grid failure_grid;                                                \
      fprintf(stderr, "tmux assertion doc=%d flags=%d: %s\n", document, flags, \
              #c);                                                             \
      if (!capture(&t, &failure_grid, 1, 0)) {                                 \
        int failure_row;                                                       \
        for (failure_row = 0; failure_row < failure_grid.count; ++failure_row) \
          fprintf(stderr, "%s\n", failure_grid.rows[failure_row]);             \
      }                                                                        \
      failed = 1;                                                              \
      goto cleanup;                                                            \
    }                                                                          \
  } while (0)

static int copy_rows(struct grid *destination, const struct grid *source,
                     int count) {
  size_t used = 0;
  int row;
  if (count < 0 || count > source->count)
    return 1;
  destination->count = count;
  for (row = 0; row < count; ++row) {
    size_t length = strlen(source->rows[row]) + 1;
    if (used + length > sizeof(destination->text))
      return 1;
    destination->rows[row] = destination->text + used;
    memcpy(destination->rows[row], source->rows[row], length);
    used += length;
  }
  return 0;
}
static int rows_equal(const struct grid *a, int first_a, const struct grid *b,
                      int first_b, int count) {
  int row;
  if (first_a < 0 || first_b < 0 || first_a + count > a->count ||
      first_b + count > b->count)
    return 0;
  for (row = 0; row < count; ++row)
    if (strcmp(a->rows[first_a + row], b->rows[first_b + row]))
      return 0;
  return 1;
}
static int has_output(const struct grid *g, int count) {
  int row;
  for (row = 0; row < count; ++row)
    if (nonblank(g->rows[row]))
      return 1;
  return 0;
}
static int wait_initial(struct terminal *t) {
  struct grid g;
  int row;
  double deadline = now() + 4;
  do {
    if (capture(t, &g, 0, 0))
      return 1;
    row = last_row(&g);
    if (row >= 0 && (!strcmp(g.rows[row], ">") || !strcmp(g.rows[row], "> ")))
      return 0;
    delay(.02);
  } while (now() < deadline);
  return 1;
}
static int resize_ack(struct terminal *t, int width, int height,
                      const char *tail) {
  double deadline = now() + 4;
  int result;
  do {
    result = exchange(t, 'r', width, height, tail);
    if (result == 0)
      return 0;
    if (result < 0)
      return 1;
    delay(.02);
  } while (now() < deadline);
  return 1;
}
static int controls_valid(const char *bytes, int shifted, int no_erase) {
  const unsigned char *p = (const unsigned char *)bytes;
  int scrolls = 0;
  while ((p = (const unsigned char *)strstr((const char *)p, "\033[")) !=
         NULL) {
    const unsigned char *start = p + 2, *end = start;
    unsigned value = 0;
    while (*end && *end >= 0x30 && *end <= 0x3f)
      ++end;
    while (*end && *end >= 0x20 && *end <= 0x2f)
      ++end;
    if (*end == 'S') {
      while (start < end && *start >= '0' && *start <= '9')
        value = value * 10 + *start++ - '0';
      scrolls += value ? (int)value : 1;
    }
    if (*end == 'T' || *end == 'L' || *end == 'M' || (no_erase && *end == 'K'))
      return 0;
    p = *end ? end + 1 : end;
  }
  return scrolls == shifted;
}
static int fixture_case(const char *tmux, const char *fixture,
                        const char *build, int document, int flags) {
  struct terminal t;
  struct grid before, native, history, actual, prefix, previous, continued;
  const char *draft_tail = flags & PAGED ? " END" : "> draft";
  const char *quote[] = {"hello", "poih", "café"};
  const char *fragments[][8] = {
      {"Heading", "A short paragraph with italic, bold, and code.", "Finish",
       "END short.", NULL, NULL, NULL, NULL},
      {"Lists", "First item has several words that wrap across columns.",
       "Second item stays distinct.", "Ordered first.", "Ordered second.",
       "A quoted paragraph.", "END lists.", NULL},
      {"Code and Unicode", "int value = 42;", "return value;",
       "Räksmörgås café é 中文 日本語.",
       "A longer paragraph has different word lengths, punctuation, and enough "
       "text to cross multiple terminal rows during the width transitions.",
       "END unicode.", NULL, NULL}};
  int failed = 0, step, width, height, owned, count, index, tail, native_cursor,
      prefix_pending = 0;
  int gap_count = 0, gap_sizes[3], shifted, actual_owned, retained, live_row,
      marker_row = -1, marker_copies = 1;
  char gaps[3][1024], draft[512], cursor[128], raw[65536], compact_text[65536],
      fragment[512], row_compact[1024];
  char flattened_before[65536], flattened_after[65536], expected[65536],
      submitted[128];
  size_t mark, starts[512], offsets[512], total, search_offset, begin,
      finish_offset;
  char *found, *last, *at;
  double deadline;
  const int normal_sizes[][2] = {{30, 18}, {80, 28}, {16, 8}, {45, 24}};
  const int height_sizes[][2] = {{80, 8}, {80, 24}, {80, 6}, {80, 24}};
  const int grow_sizes[][2] = {{80, 28},  {120, 36}, {160, 72},
                               {160, 96}, {80, 48},  {120, 96}};
  const int quote_sizes[][2] = {{40, 24}, {20, 24}, {8, 24}, {80, 28}};
  const int (*sizes)[2] = flags & QUOTED      ? quote_sizes
                          : flags & GROW      ? grow_sizes
                          : flags & ROUNDTRIP ? height_sizes
                                              : normal_sizes;
  CHECK(prepare(&t, tmux, build, "fixture") == 0);
  CHECK(launch(&t, "fixture", fixture, document, flags & PREFILLED,
               flags & GROW ? 30 : 80, flags & GROW ? 12 : 24, 0, 0) == 0);
  CHECK(wait_initial(&t) == 0);
  strcpy(draft, "draft");
  if (flags & PAGED) {
    strcat(draft, " ");
    memset(draft + 6, 'x', 250);
    strcpy(draft + 256, " END");
  }
  CHECK(command(&t, NULL, 0, "send-keys", "-t", t.pane, "-l", draft,
                (const char *)NULL) == 0);
  CHECK(exchange(&t, 'r', 0, 0, draft_tail) == 0);
  if (document == 2)
    CHECK(exchange(&t, 'q', 0, 0, draft_tail) == 0);
  for (step = 0; step < 200; ++step) {
    if ((flags & LIVE) &&
        (step == 5 || step == 10 || step == 15 || step == 22)) {
      height = step == 5 ? 8 : step == 15 ? 6 : 24;
      CHECK(resize_window(&t, 80, height) == 0);
      CHECK(resize_ack(&t, 80, height, draft_tail) == 0);
    }
    if ((flags & RESIZE) && !(flags & (STATIC | LIVE)) &&
        (step == 2 || step == 5 || step == 9)) {
      width = step == 2 ? 45 : step == 5 ? 30 : 80;
      height = flags & HEIGHT ? (step == 5 ? 18 : step == 9 ? 28 : 24) : 24;
      CHECK(capture(&t, &before, 0, 0) == 0);
      CHECK(exchange(&t, 'b', 0, 0, draft_tail) == 0);
      if (resize_window(&t, width, height) || wait_tty(&t, width, height) ||
          capture(&t, &native, 0, 0) || capture(&t, &history, 1, 0)) {
        write(t.command_fd, "b", 1);
        CHECK(0);
      }
      if (t.line_open && has_output(&before, owned_row(&before, 1)) &&
          !has_output(&native, owned_row(&native, 1))) {
        CHECK(copy_rows(&prefix, &history, history.count - native.count) == 0);
        prefix_pending = 1;
        for (tail = prefix.count - 1; tail >= 0 && !nonblank(prefix.rows[tail]);
             --tail) {
        }
        if (tail >= 0) {
          CHECK(gap_count < 3);
          compact(prefix.rows[tail], gaps[gap_count]);
          gap_sizes[gap_count++] = prefix.count - tail - 1;
        }
      }
      CHECK(write(t.command_fd, "b", 1) == 1);
      CHECK(resize_ack(&t, width, height, draft_tail) == 0);
    }
    CHECK(exchange(&t, 's', 0, 0, draft_tail) == 0);
    if (prefix_pending) {
      CHECK(capture(&t, &continued, 0, 0) == 0);
      if (has_output(&continued, owned_row(&continued, 1))) {
        CHECK(nonblank(continued.rows[0]));
        CHECK(capture(&t, &history, 1, 0) == 0);
        CHECK(rows_equal(&history, 0, &prefix, 0, prefix.count));
        prefix_pending = 0;
      }
    }
    if (t.reply[0] == 'd')
      break;
  }
  CHECK(step < 200);
  CHECK(exchange(&t, 'f', 0, 0, draft_tail) == 0);
  CHECK(!prefix_pending);
  if (flags & QUOTED)
    CHECK(exchange(&t, 'h', 0, 0, draft_tail) == 0);
  if (flags & STATIC) {
    if (flags & GROW) {
      CHECK(exchange(&t, 'u', 0, 0, draft_tail) == 0);
      CHECK(exchange(&t, 'p', 0, 0, draft_tail) == 0);
    }
    if (flags & ROUNDTRIP) {
      CHECK(exchange(&t, 'p', 0, 0, draft_tail) == 0);
      CHECK(capture(&t, &native, 0, 0) == 0);
      for (index = 0; index < native.count; ++index)
        if (strstr(native.rows[index], "AFTER RESIZE")) {
          marker_row = index;
          break;
        }
      CHECK(marker_row >= 0);
    }
    for (step = 0; step < (flags & GROW ? 6 : 4); ++step) {
      width = sizes[step][0];
      height = sizes[step][1];
      CHECK(exchange(&t, 'b', 0, 0, draft_tail) == 0);
      if (resize_window(&t, width, height) || wait_tty(&t, width, height) ||
          capture(&t, &native, 0, 0) ||
          query(&t, "#{cursor_y}", cursor, sizeof(cursor))) {
        write(t.command_fd, "b", 1);
        CHECK(0);
      }
      native_cursor = atoi(cursor);
      t.expected_prompt = height - 1;
      owned = owned_row(&native, 0);
      CHECK(recorded_frame(&t, 0, raw, sizeof(raw)) == 0);
      mark = recording_size(&t);
      CHECK(write(t.command_fd, "b", 1) == 1);
      CHECK(exchange(&t, 'v', width, height, draft_tail) == 0);
      CHECK(capture(&t, &actual, 0, 0) == 0);
      actual_owned = owned_row(&actual, 0);
      retained = owned < actual_owned ? owned : actual_owned;
      shifted = 0;
      if (flags & QUOTED) {
        for (live_row = 0;
             live_row < native.count && strcmp(native.rows[live_row], "~");
             ++live_row) {
        }
        CHECK(live_row < native.count);
        shifted = live_row >= retained ? live_row - retained + 1 : 0;
      }
      if (flags & PAGED) {
        count = owned_row(&actual, 1);
        if (retained > count)
          retained = count;
        for (live_row = native.count - 1;
             live_row >= 0 && !strstr(native.rows[live_row], "AFTER RESIZE");
             --live_row) {
        }
        shifted = live_row >= retained ? live_row - retained + 1 : 0;
      }
      CHECK(rows_equal(&actual, 0, &native, shifted, retained));
      if (flags & GROW) {
        /* Moving the owned frame must leave its old rows empty, while the
         * output prefix above it remains byte-for-byte native content. */
        for (index = owned; index <= native_cursor && index < actual_owned;
             ++index)
          CHECK(!nonblank(actual.rows[index]));
        CHECK(rows_equal(&actual, native_cursor + 1, &native, native_cursor + 1,
                         actual_owned > native_cursor
                             ? actual_owned - native_cursor - 1
                             : 0));
      }
      CHECK(recorded_frame(&t, mark, raw, sizeof(raw)) == 0);
      if (flags & QUOTED) {
        CHECK(capture(&t, &history, 1, 0) == 0);
        snprintf(submitted, sizeof(submitted), "> %s", quote[document]);
        count = 0;
        for (index = 0; index < history.count; ++index)
          if (strstr(history.rows[index], submitted)) {
            CHECK(!strcmp(history.rows[index], submitted));
            ++count;
          }
        CHECK(count == 1 && !strstr(raw, quote[document]));
      }
      CHECK(controls_valid(raw, shifted,
                           (flags & ROUNDTRIP) && !(flags & PAGED) &&
                               native_cursor == t.expected_prompt));
      if ((flags & ROUNDTRIP) && !(flags & PAGED) &&
          native_cursor == t.expected_prompt)
        CHECK(!strstr(raw, "> draft") && !strstr(raw, "+ fixture"));
      if (flags & GROW) {
        CHECK(capture(&t, &history, 1, 0) == 0);
        flatten(&history, flattened_before, 0);
        CHECK(exchange(&t, 'p', 0, 0, draft_tail) == 0);
        CHECK(capture(&t, &history, 1, 0) == 0);
        flatten(&history, flattened_after, 0);
        last = NULL;
        for (at = flattened_before; (at = strstr(at, "AFTER RESIZE")) != NULL;
             ++at)
          last = at;
        CHECK(last);
        begin = (size_t)(last - flattened_before) + strlen("AFTER RESIZE");
        CHECK(strlen(flattened_before) + 13 < sizeof(expected));
        memcpy(expected, flattened_before, begin);
        expected[begin] = 0;
        strcat(expected, "AFTER RESIZE");
        strcat(expected, flattened_before + begin);
        CHECK(!strcmp(expected, flattened_after));
      }
      if ((flags & ROUNDTRIP) && (height == 24 || (flags & PAGED))) {
        CHECK(capture(&t, &previous, 0, 0) == 0);
        live_row = -1;
        if (flags & PAGED)
          for (index = previous.count - 1; index >= 0; --index)
            if (strstr(previous.rows[index], "AFTER RESIZE")) {
              live_row = index;
              break;
            }
        CHECK(exchange(&t, 'p', 0, 0, draft_tail) == 0);
        CHECK(capture(&t, &continued, 0, 0) == 0);
        for (index = 0; index < retained; ++index)
          if (nonblank(previous.rows[index]))
            CHECK(!strncmp(continued.rows[index], previous.rows[index],
                           strlen(previous.rows[index])));
        ++marker_copies;
        if (flags & PAGED) {
          if (live_row >= 0) {
            strcpy(expected, previous.rows[live_row]);
            strcat(expected, "AFTER RESIZE");
            CHECK(!strcmp(continued.rows[live_row], expected));
          }
        } else {
          expected[0] = 0;
          for (index = 0; index < marker_copies; ++index)
            strcat(expected, "AFTER RESIZE");
          CHECK(!strcmp(continued.rows[marker_row], expected));
        }
      }
    }
    goto cleanup;
  }
  CHECK(capture(&t, &history, 1, 0) == 0);
  total = 0;
  for (index = 0; index < history.count; ++index) {
    compact(history.rows[index], row_compact);
    starts[index] = total;
    offsets[index] = strlen(row_compact);
    CHECK(total + offsets[index] < sizeof(compact_text));
    memcpy(compact_text + total, row_compact, offsets[index]);
    total += offsets[index];
  }
  compact_text[total] = 0;
  search_offset = 0;
  for (count = 0; count < 8 && fragments[document][count]; ++count) {
    compact(fragments[document][count], fragment);
    found = strstr(compact_text + search_offset, fragment);
    CHECK(found);
    begin = (size_t)(found - compact_text);
    finish_offset = begin + strlen(fragment);
    search_offset = finish_offset;
    if (!(flags & LIVE))
      for (index = 0; index < history.count; ++index) {
        if (starts[index] <= begin || starts[index] >= finish_offset ||
            offsets[index])
          continue;
        for (tail = index - 1; tail >= 0 && !offsets[tail]; --tail) {
        }
        for (owned = index + 1; owned < history.count && !offsets[owned];
             ++owned) {
        }
        CHECK(tail >= 0 && owned < history.count);
        compact(history.rows[tail], row_compact);
        for (step = 0; step < gap_count && strcmp(row_compact, gaps[step]);
             ++step) {
        }
        CHECK(step < gap_count && gap_sizes[step] == owned - tail - 1);
      }
  }
  flatten(&history, flattened_before, 0);
  CHECK(!strstr(flattened_before, "Thinking..."));
  count = 0;
  for (index = 0; index < history.count; ++index)
    if (strstr(history.rows[index], "> draft"))
      ++count;
  CHECK(count == 1);
  CHECK(exchange(&t, 'g', 0, 0, draft_tail) == 0);
  CHECK(query(&t, "#{cursor_y}", cursor, sizeof(cursor)) == 0);
  strcpy(submitted, cursor);
  CHECK(command(&t, NULL, 0, "send-keys", "-t", t.pane, "C-u", "C-d",
                (const char *)NULL) == 0);
  deadline = now() + 4;
  do {
    CHECK(query(&t, "#{pane_dead}", cursor, sizeof(cursor)) == 0);
    if (!strcmp(cursor, "1"))
      break;
    delay(.02);
  } while (now() < deadline);
  CHECK(!strcmp(cursor, "1"));
  delay(.05);
  CHECK(recording(&t, 0, raw, sizeof(raw)) == 0);
  CHECK(!strstr(raw, "\033[?25l"));
  CHECK(command(&t, raw, sizeof(raw), "capture-pane", "-p", "-t", t.pane, "-S",
                submitted, "-E", submitted, (const char *)NULL) == 0);
  CHECK(!strcmp(raw, "\n") || !*raw);
  strcat(submitted, ",0");
  CHECK(query(&t, "#{cursor_y},#{cursor_x}", cursor, sizeof(cursor)) == 0);
  CHECK(!strcmp(cursor, submitted));
cleanup:
  cleanup(&t);
  return failed;
}
static int producer_case(const char *tmux, const char *fixture,
                         const char *build, int document, int prefilled,
                         int sequence) {
  struct terminal t;
  struct grid before, after, expected;
  const char *sources[] = {"abcdefghijklmnopqrstuvwxyz", "one\nABC",
                           "café 中文"};
  const int mixed[][2] = {{40, 14}, {18, 6}, {48, 16}, {20, 10}};
  int failed = 0, flags = prefilled, stage, width, height, row;
  char compact_text[65536], cursor[128];
  double deadline;
  CHECK(prepare(&t, tmux, build, "producer") == 0);
  CHECK(launch(&t, "producer", fixture, document, prefilled, 20, 10, 1, 0) ==
        0);
  CHECK(ack(&t) == 0 && t.reply[0] == 'a');
  CHECK(write(t.command_fd, "s", 1) == 1);
  CHECK(ack(&t) == 0 && t.reply[0] == 'a');
  deadline = now() + 4;
  do {
    CHECK(capture(&t, &before, 1, 1) == 0);
    flatten(&before, compact_text, 0);
    if (document == 1 ? strstr(compact_text, "oneABC") != NULL
                      : strstr(compact_text, sources[document]) != NULL)
      break;
    delay(.02);
  } while (now() < deadline);
  CHECK(now() < deadline);
  for (stage = 0; stage < (sequence == 3 ? 4 : 1); ++stage) {
    width = sequence == 0 ? 40 : sequence == 3 ? mixed[stage][0] : 20;
    height = sequence == 0   ? 10
             : sequence == 1 ? 14
             : sequence == 2 ? 6
                             : mixed[stage][1];
    CHECK(resize_window(&t, width, height) == 0);
    deadline = now() + 4;
    do {
      CHECK(write(t.command_fd, "r", 1) == 1);
      CHECK(ack(&t) == 0 && t.reply[0] == 'a');
      if (t.reply[1] * 256 + t.reply[2] == width &&
          t.reply[3] * 256 + t.reply[4] == height)
        break;
      delay(.02);
    } while (now() < deadline);
    CHECK(now() < deadline);
    CHECK(capture(&t, &before, 1, 1) == 0);
    while (before.count && !*before.rows[before.count - 1])
      --before.count;
    CHECK(copy_rows(&expected, &before, before.count) == 0 &&
          expected.count > 0);
    strcat(expected.rows[expected.count - 1], "END");
    CHECK(write(t.command_fd, "p", 1) == 1);
    CHECK(ack(&t) == 0);
    deadline = now() + 4;
    do {
      CHECK(capture(&t, &after, 1, 1) == 0);
      while (after.count && !*after.rows[after.count - 1])
        --after.count;
      if (expected.count == after.count &&
          rows_equal(&expected, 0, &after, 0, expected.count))
        break;
      delay(.02);
    } while (now() < deadline);
    CHECK(now() < deadline);
  }
  CHECK(write(t.command_fd, "w", 1) == 1);
  CHECK(ack(&t) == 0);
  CHECK(capture(&t, &after, 1, 1) == 0);
  while (after.count && !*after.rows[after.count - 1])
    --after.count;
  CHECK(expected.count == after.count &&
        rows_equal(&expected, 0, &after, 0, expected.count));
  strcat(expected.rows[expected.count - 1], "END");
  CHECK(write(t.command_fd, "p", 1) == 1);
  CHECK(ack(&t) == 0);
  deadline = now() + 4;
  do {
    CHECK(capture(&t, &after, 1, 1) == 0);
    while (after.count && !*after.rows[after.count - 1])
      --after.count;
    if (expected.count == after.count &&
        rows_equal(&expected, 0, &after, 0, expected.count))
      break;
    delay(.02);
  } while (now() < deadline);
  CHECK(now() < deadline);
  CHECK(write(t.command_fd, "i", 1) == 1);
  CHECK(wait_initial(&t) == 0);
  CHECK(command(&t, NULL, 0, "send-keys", "-t", t.pane, "-l", "draft",
                (const char *)NULL) == 0);
  deadline = now() + 4;
  do {
    CHECK(capture(&t, &after, 1, 1) == 0);
    row = last_row(&after);
    if (row >= 0 && !strcmp(after.rows[row], "> draft"))
      break;
    delay(.02);
  } while (now() < deadline);
  CHECK(now() < deadline);
  after.count = row;
  while (after.count && !*after.rows[after.count - 1])
    --after.count;
  CHECK(after.count == expected.count &&
        rows_equal(&after, 0, &expected, 0, expected.count));
  CHECK(query(&t, "#{cursor_y},#{pane_height}", cursor, sizeof(cursor)) == 0);
  CHECK(sscanf(cursor, "%d,%d", &row, &height) == 2 && row == height - 1);
  CHECK(command(&t, NULL, 0, "send-keys", "-t", t.pane, "Enter",
                (const char *)NULL) == 0);
  CHECK(ack(&t) == 0);
  CHECK(write(t.command_fd, "x", 1) == 1);
  deadline = now() + 4;
  do {
    CHECK(query(&t, "#{pane_dead}", cursor, sizeof(cursor)) == 0);
    if (!strcmp(cursor, "1"))
      break;
    delay(.02);
  } while (now() < deadline);
  CHECK(!strcmp(cursor, "1"));
  CHECK(query(&t, "#{pane_dead_status}", cursor, sizeof(cursor)) == 0);
  CHECK(!strcmp(cursor, "0"));
cleanup:
  cleanup(&t);
  return failed;
}
static int cursor_input(struct terminal *t, const struct grid *g) {
  char cursor[128];
  int row, column, visible, last = last_row(g);
  if (last < 0 || query(t, "#{cursor_y},#{cursor_x},#{cursor_flag}", cursor,
                        sizeof(cursor)))
    return 0;
  return sscanf(cursor, "%d,%d,%d", &row, &column, &visible) == 3 &&
         row == last && column == (int)strlen(g->rows[last]) && visible == 1;
}
static int wait_example(struct terminal *t, const char *marker,
                        const char *tail, size_t mark, int cpr, int cursor,
                        int complete) {
  struct grid g;
  char raw[65536], text[65536];
  int last;
  double deadline = now() + 4;
  do {
    if (capture(t, &g, 0, 0))
      return 1;
    flatten(&g, text, 0);
    last = last_row(&g);
    if ((!marker || strstr(text, marker)) &&
        (!tail ||
         (last >= 0 && strlen(g.rows[last]) >= strlen(tail) &&
          !strcmp(g.rows[last] + strlen(g.rows[last]) - strlen(tail), tail))) &&
        (!cursor || cursor_input(t, &g)) &&
        (!complete || !strstr(text, "Reasoning"))) {
      if (!cpr)
        return 0;
      if (recording(t, mark, raw, sizeof(raw)))
        return 1;
      if (strstr(raw, "\033[6n"))
        return 0;
    }
    delay(.02);
  } while (now() < deadline);
  fprintf(stderr, "Example tmux frame missing: %s / %s\n", marker ? marker : "",
          tail ? tail : "");
  return 1;
}
static int example_case(const char *tmux, const char *example,
                        const char *build, int height_only, int prefilled,
                        int live_resize, int paced) {
  struct terminal t;
  struct grid g;
  int failed = 0, document = -1, flags = height_only, stage, width, height;
  char draft[128], raw[65536], text[65536], cursor[128], exit_row[32],
      expected[64];
  size_t mark;
  double deadline;
  const int input_sizes[][2] = {{30, 24}, {80, 24}, {9, 3},
                                {35, 18}, {80, 28}, {30, 24}};
  const int live_sizes[][2] = {{45, 24}, {30, 18}, {80, 28}};
  const int height_sizes[][2] = {{80, 8}, {80, 24}, {80, 6}, {80, 24}};
  CHECK(prepare(&t, tmux, build, "chat") == 0);
  CHECK(launch(&t, "chat", example, 0, prefilled, 80, 24, 0, 5) == 0);
  CHECK(wait_initial(&t) == 0);
  strcpy(draft, "draft ");
  memset(draft + 6, 'x', 93);
  strcpy(draft + 99, " END");
  CHECK(command(&t, NULL, 0, "send-keys", "-t", t.pane, "-l", draft,
                (const char *)NULL) == 0);
  CHECK(wait_example(&t, NULL, "END", 0, 0, 0, 0) == 0);
  for (stage = 0; !height_only && stage < 6; ++stage) {
    if (paced)
      delay(.3);
    mark = recording_size(&t);
    CHECK(resize_window(&t, input_sizes[stage][0], input_sizes[stage][1]) == 0);
    CHECK(wait_example(&t, NULL, "END", mark, 1, 1, 0) == 0);
  }
  CHECK(command(&t, NULL, 0, "send-keys", "-t", t.pane, "C-u",
                (const char *)NULL) == 0);
  CHECK(command(&t, NULL, 0, "send-keys", "-t", t.pane, "-l", "hello",
                (const char *)NULL) == 0);
  CHECK(command(&t, NULL, 0, "send-keys", "-t", t.pane, "Enter",
                (const char *)NULL) == 0);
  CHECK(wait_example(&t, "A short answer", NULL, 0, 0, 0, 0) == 0);
  CHECK(command(&t, NULL, 0, "send-keys", "-t", t.pane, "-l", "draft",
                (const char *)NULL) == 0);
  for (stage = 0; live_resize && stage < (height_only ? 4 : 3); ++stage) {
    width = height_only ? height_sizes[stage][0] : live_sizes[stage][0];
    height = height_only ? height_sizes[stage][1] : live_sizes[stage][1];
    if (paced)
      delay(.3);
    mark = recording_size(&t);
    CHECK(resize_window(&t, width, height) == 0);
    CHECK(wait_example(&t, NULL, "> draft", mark, 1, 1, 0) == 0);
    if (height_only)
      delay(.15);
  }
  if (!height_only)
    CHECK(wait_example(&t, "Next step", NULL, 0, 0, 0, 0) == 0);
  CHECK(wait_example(&t, "Try another prompt", NULL, 0, 0, 0, 1) == 0);
  CHECK(capture(&t, &g, 1, 0) == 0);
  flatten(&g, text, 1);
  CHECK(
      strstr(text, "Hereisitaliccontext,boldemphasis,andcodeinoneparagraph."));
  delay(.1);
  CHECK(recording(&t, 0, raw, sizeof(raw)) == 0);
  CHECK(!strstr(raw, "\033[?25l"));
  CHECK(query(&t, "#{cursor_y}", exit_row, sizeof(exit_row)) == 0);
  CHECK(command(&t, NULL, 0, "send-keys", "-t", t.pane, "C-u",
                (const char *)NULL) == 0);
  CHECK(command(&t, NULL, 0, "send-keys", "-t", t.pane, "-l", "/quit",
                (const char *)NULL) == 0);
  CHECK(command(&t, NULL, 0, "send-keys", "-t", t.pane, "Enter",
                (const char *)NULL) == 0);
  deadline = now() + 4;
  snprintf(expected, sizeof(expected), "%s,0", exit_row);
  do {
    CHECK(capture(&t, &g, 0, 0) == 0);
    flatten(&g, text, 0);
    CHECK(query(&t, "#{pane_dead}", cursor, sizeof(cursor)) == 0);
    if (!strcmp(cursor, "1") && strstr(text, "! Goodbye.") &&
        !strstr(text, "> /quit")) {
      CHECK(command(&t, raw, sizeof(raw), "capture-pane", "-p", "-t", t.pane,
                    "-S", exit_row, "-E", exit_row, (const char *)NULL) == 0);
      CHECK(query(&t, "#{cursor_y},#{cursor_x}", cursor, sizeof(cursor)) == 0);
      if ((!*raw || !strcmp(raw, "\n")) && !strcmp(cursor, expected))
        break;
    }
    delay(.02);
  } while (now() < deadline);
  CHECK(now() < deadline);
cleanup:
  cleanup(&t);
  return failed;
}
static int wait_bottom_frame(struct terminal *t, int width, int height) {
  char actual[128], expected[128];
  double deadline = now() + 1;
  snprintf(expected, sizeof(expected), "%d,%d,%d,%d", width, height, height - 1,
           height - 4);
  do {
    if (query(t,
              "#{pane_width},#{pane_height},#{cursor_y},#{scroll_region_lower}",
              actual, sizeof(actual)))
      return 1;
    if (!strcmp(actual, expected))
      return 0;
    delay(.01);
  } while (now() < deadline);
  fprintf(stderr, "Resize frame width,height,cursor,margin: %s; expected %s\n",
          actual, expected);
  return 1;
}

static int single_grow_case(const char *tmux, const char *example,
                            const char *build, int running) {
  struct terminal t;
  struct grid g;
  int failed = 0, document = -1, flags = running, row, prompts, banners;
  char cursor[32], before[65536], after[65536];
  const char *draft = "keep café draft";
  CHECK(prepare(&t, tmux, build, "grow") == 0);
  CHECK(launch(&t, "grow", example, 0, 0, 120, 24, 0, running ? 1000 : 5) == 0);
  CHECK(command(&t, NULL, 0, "set-option", "-g", "status", "off",
                (const char *)NULL) == 0);
  CHECK(wait_initial(&t) == 0);
  if (running) {
    CHECK(command(&t, NULL, 0, "send-keys", "-t", t.pane, "grow-check", "Enter",
                  (const char *)NULL) == 0);
    CHECK(wait_example(&t, "Thinking...", ">", 0, 0, 0, 0) == 0);
  }
  CHECK(command(&t, NULL, 0, "send-keys", "-t", t.pane, "-l", draft,
                (const char *)NULL) == 0);
  CHECK(wait_example(&t, NULL, draft, 0, 0, 0, 0) == 0);
  CHECK(capture(&t, &g, 0, 1) == 0 && g.count >= 3);
  g.count -= 3;
  flatten(&g, before, 1);
  CHECK(before[0]);
  /* Start with little scrollback, then let each preceding size settle.
   * The last growth gets no keypress, producer command or second resize. */
  CHECK(resize_window(&t, 240, 80) == 0);
  CHECK(wait_tty(&t, 240, 80) == 0);
  delay(.3);
  CHECK(resize_window(&t, 40, 8) == 0);
  CHECK(wait_tty(&t, 40, 8) == 0);
  delay(.3);
  CHECK(resize_window(&t, 240, 80) == 0);
  CHECK(wait_tty(&t, 240, 80) == 0);
  CHECK(wait_bottom_frame(&t, 240, 80) == 0);
  if (running)
    CHECK(wait_example(&t, "# A", draft, 0, 0, 0, 0) == 0);
  CHECK(capture(&t, &g, 0, 0) == 0);
  CHECK(!strcmp(g.rows[79], "> keep café draft"));
  prompts = 0;
  for (row = 0; row < g.count; ++row)
    prompts += strstr(g.rows[row], "keep café draft") != NULL;
  CHECK(prompts == 1);
  CHECK(query(&t, "#{cursor_x}", cursor, sizeof(cursor)) == 0);
  CHECK(!strcmp(cursor, "17"));
  CHECK(capture(&t, &g, 1, 1) == 0);
  flatten(&g, after, 1);
  CHECK(!strncmp(after, before, strlen(before)));
  banners = 0;
  for (row = 0; row < g.count; ++row)
    banners += strstr(g.rows[row], "Enter sends or queues;") != NULL;
  CHECK(banners == 1);
  CHECK(command(&t, NULL, 0, "send-keys", "-t", t.pane, "-l", "!",
                (const char *)NULL) == 0);
  CHECK(wait_example(&t, NULL, "keep café draft!", 0, 0, 0, 0) == 0);
  CHECK(wait_bottom_frame(&t, 240, 80) == 0);
cleanup:
  cleanup(&t);
  return failed;
}

static int streaming_tiny_case(const char *tmux, const char *example,
                               const char *build) {
  struct terminal t;
  struct grid g;
  int failed = 0, document = -1, flags = 0, continuation_row, row;
  char parent[32], path[128], expected[512], *italic;
  FILE *children;
  long child;
  pid_t worker = -1;
  CHECK(prepare(&t, tmux, build, "tiny") == 0);
  CHECK(launch(&t, "tiny", example, 0, 0, 120, 80, 0, 5) == 0);
  CHECK(command(&t, NULL, 0, "set-option", "-g", "status", "off",
                (const char *)NULL) == 0);
  CHECK(wait_initial(&t) == 0);
  CHECK(command(&t, NULL, 0, "send-keys", "-t", t.pane, "one", "Enter",
                (const char *)NULL) == 0);
  CHECK(wait_example(&t, "Here is", ">", 0, 0, 0, 0) == 0);
  CHECK(resize_window(&t, 120, 3) == 0 && wait_tty(&t, 120, 3) == 0);
  CHECK(wait_example(&t, "italic", ">", 0, 0, 0, 0) == 0);
  CHECK(query(&t, "#{pane_pid}", parent, sizeof(parent)) == 0);
  snprintf(path, sizeof(path), "/proc/%s/task/%s/children", parent, parent);
  children = fopen(path, "r");
  CHECK(children != NULL);
  row = fscanf(children, "%ld", &child);
  fclose(children);
  CHECK(row == 1 && child > 0);
  worker = (pid_t)child;
  CHECK(kill(worker, SIGSTOP) == 0);
  delay(.1);
  CHECK(resize_window(&t, 120, 80) == 0 && wait_tty(&t, 120, 80) == 0);
  CHECK(wait_bottom_frame(&t, 120, 80) == 0);
  CHECK(capture(&t, &g, 1, 1) == 0);
  continuation_row = -1;
  for (row = 0; row < g.count; ++row)
    if ((italic = strstr(g.rows[row], "italic")) != NULL) {
      CHECK(continuation_row < 0);
      continuation_row = row;
      CHECK((size_t)(italic - g.rows[row]) + 80 < sizeof(expected));
      memcpy(expected, g.rows[row], (size_t)(italic - g.rows[row]));
      strcpy(expected + (italic - g.rows[row]),
             "italic context, bold emphasis, and code in one paragraph.");
    }
  CHECK(continuation_row >= 0);
  CHECK(kill(worker, SIGCONT) == 0);
  worker = -1;
  CHECK(wait_example(&t, "Try another prompt.", ">", 0, 0, 0, 0) == 0);
  CHECK(wait_bottom_frame(&t, 120, 80) == 0);
  CHECK(capture(&t, &g, 1, 1) == 0);
  CHECK(continuation_row < g.count);
  if (strcmp(g.rows[continuation_row], expected))
    fprintf(stderr, "Expected cursor continuation: |%s|\nActual: |%s|\n",
            expected, g.rows[continuation_row]);
  CHECK(!strcmp(g.rows[continuation_row], expected));
cleanup:
  if (worker > 0)
    kill(worker, SIGCONT);
  cleanup(&t);
  return failed;
}

int main(int argc, char **argv) {
  const char *mode;
  int document, prefilled, sequence;
  if (argc < 5 || argc > 6)
    return 2;
  mode = argc == 6 ? argv[5] : "default";
  if (!strcmp(mode, "--streaming-tiny"))
    return streaming_tiny_case(argv[1], argv[2], argv[3]);
  if (!strcmp(mode, "--single-grow-idle") ||
      !strcmp(mode, "--single-grow-running"))
    return single_grow_case(argv[1], argv[2], argv[3],
                            !strcmp(mode, "--single-grow-running"));
  if (!strcmp(mode, "--producer-only")) {
    for (document = 0; document < 3; ++document)
      for (prefilled = 0; prefilled < 2; ++prefilled)
        for (sequence = 0; sequence < 4; ++sequence)
          if (producer_case(argv[1], argv[4], argv[3], document, prefilled,
                            sequence))
            return 1;
  } else if (!strcmp(mode, "--grow-both") || !strcmp(mode, "--quoted")) {
    for (document = 0; document < 3; ++document)
      for (prefilled = 0; prefilled < 2; ++prefilled)
        if (fixture_case(argv[1], argv[4], argv[3], document,
                         RESIZE | STATIC | (prefilled ? PREFILLED : 0) |
                             (!strcmp(mode, "--quoted") ? QUOTED : GROW)))
          return 1;
  } else if (!strcmp(mode, "--static") || !strcmp(mode, "--height-roundtrip") ||
             !strcmp(mode, "--prefilled")) {
    int roundtrip = strcmp(mode, "--static") != 0;
    for (document = 0; document < 3; ++document) {
      if (fixture_case(argv[1], argv[4], argv[3], document,
                       RESIZE | STATIC | (roundtrip ? ROUNDTRIP : 0) |
                           (!strcmp(mode, "--prefilled") ? PREFILLED : 0)))
        return 1;
      if (roundtrip) {
        if (fixture_case(argv[1], argv[4], argv[3], document,
                         RESIZE | LIVE |
                             (!strcmp(mode, "--prefilled") ? PREFILLED : 0)))
          return 1;
        if (strcmp(mode, "--prefilled") &&
            fixture_case(argv[1], argv[4], argv[3], document,
                         RESIZE | STATIC | ROUNDTRIP | PAGED))
          return 1;
      }
    }
    if (!strcmp(mode, "--prefilled") &&
        example_case(argv[1], argv[2], argv[3], 1, 1, 1, 1))
      return 1;
  } else if (!strcmp(mode, "--no-resize"))
    return example_case(argv[1], argv[2], argv[3], 1, 1, 0, 1);
  else if (!strcmp(mode, "--queued-resize"))
    return example_case(argv[1], argv[2], argv[3], 0, 0, 1, 0);
  else if (!strcmp(mode, "default")) {
    for (document = 0; document < 3; ++document) {
      if (fixture_case(argv[1], argv[4], argv[3], document, 0) ||
          fixture_case(argv[1], argv[4], argv[3], document, RESIZE) ||
          fixture_case(argv[1], argv[4], argv[3], document, RESIZE | HEIGHT) ||
          fixture_case(argv[1], argv[4], argv[3], document,
                       RESIZE | HEIGHT | PREFILLED))
        return 1;
    }
    return example_case(argv[1], argv[2], argv[3], 0, 0, 1, 1);
  } else
    return 2;
  return 0;
}
