#ifndef SOFTLINE_EXAMPLE_TRANSCRIPT_DEMO_H
#define SOFTLINE_EXAMPLE_TRANSCRIPT_DEMO_H
#include <stdio.h>

#define DEMO_TRANSCRIPT_ROWS 12
#define DEMO_TRANSCRIPT_COLUMNS 64

static const char *const demo_transcript_titles[] = {"Run log",
                                                     "Worker screen"};

static void demo_transcript_frame(int index, unsigned long turns, char *out,
                                  size_t capacity) {
  if (index == 0)
    (void)snprintf(out, capacity, "Chat example\nResponses received: %lu\n",
                   turns);
  else
    (void)snprintf(out, capacity,
                   "\033[?1049h\033[H\033[2J\033[1;36mWorker screen\033[0m"
                   "\033[3;1HResponses received: %lu",
                   turns);
}
#endif
