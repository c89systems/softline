#ifndef SOFTLINE_EXAMPLE_TRANSCRIPT_DEMO_H
#define SOFTLINE_EXAMPLE_TRANSCRIPT_DEMO_H
#include <stdio.h>
#include <time.h>

#define DEMO_TRANSCRIPT_ROWS 12
#define DEMO_TRANSCRIPT_COLUMNS 144
#define DEMO_TRANSCRIPT_SEED 32ul
#define DEMO_TRANSCRIPT_REPLAY 48ul
#define DEMO_TRANSCRIPT_LINE 144
#define DEMO_TRANSCRIPT_INTERVAL 0.4
#define DEMO_TRANSCRIPT_HISTORY                                                \
  (DEMO_TRANSCRIPT_REPLAY * DEMO_TRANSCRIPT_LINE + 64)

static const char *const demo_transcript_titles[] = {"Run log",
                                                     "Worker screen"};

static double demo_monotonic_seconds(void) {
  struct timespec value;
  if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
    return -1;
  return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
}

static int demo_transcript_due(double now, double *next) {
  if (now < 0 || now < *next)
    return 0;
  *next = now + DEMO_TRANSCRIPT_INTERVAL;
  return 1;
}

static int demo_transcript_line(unsigned long step, char *out,
                                size_t capacity) {
  static const char *const phases[] = {"cache hit", "compiling", "checking",
                                       "linking", "test passed"};
  return snprintf(out, capacity,
                  "demo step %06lu  %-12s  worker=%lu  "
                  "artifact=build/debug/transcript/objects/example-%02lu.o  "
                  "elapsed=%lu ms\n",
                  step, phases[step % 5], step % 3 + 1, step % 100,
                  step % 53 + 7);
}

static void demo_transcript_history(unsigned long step, unsigned long turns,
                                    char out[DEMO_TRANSCRIPT_HISTORY]) {
  unsigned long count =
      step < DEMO_TRANSCRIPT_REPLAY ? step : DEMO_TRANSCRIPT_REPLAY;
  size_t offset =
      (size_t)snprintf(out, DEMO_TRANSCRIPT_HISTORY,
                       "Chat example\nResponses received: %lu\n", turns);
  unsigned long left;
  for (left = count; left > 0; --left)
    offset += (size_t)demo_transcript_line(step - left + 1, out + offset,
                                           DEMO_TRANSCRIPT_HISTORY - offset);
}

static void demo_transcript_worker(unsigned long step, unsigned long turns,
                                   int initial, char *out, size_t capacity) {
  (void)snprintf(out, capacity,
                 "%s\033[H\033[1;36mWorker screen\033[0m"
                 "\033[3;1HResponses received: %lu\033[K"
                 "\033[4;1HBackground steps: %lu\033[K",
                 initial ? "\033[?1049h\033[2J" : "", turns, step);
}
#endif
