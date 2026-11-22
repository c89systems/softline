#ifndef SOFTLINE_CHAT_DEMO_DATA_H
#define SOFTLINE_CHAT_DEMO_DATA_H
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define DEMO_STREAM_DEFAULT_DELAY_US 20000u
#define DEMO_STREAM_LLM_BYTES 4
#define DEMO_STREAM_DIRECT_BYTES 64
enum demo_stream_mode {
  DEMO_STREAM_INVALID = -1,
  DEMO_STREAM_DEFAULT = 0,
  DEMO_STREAM_DIRECT,
  DEMO_STREAM_LLM
};
static const char *const response_templates[] = {
    "# A short answer\n\nHere is *italic* context, **bold** emphasis, and "
    "`code` in one paragraph.\n\n## Next step\n\nTry another prompt.\n",
    "## A longer answer\n\nFirst, inspect the input.\n\nThen use `code` "
    "for the operation; **bold** marks the result and *italic* marks a "
    "caveat.\n\n# Summary\n\nThe stream is still live.\n",
    "# Notes\n\nA single line can be *italic*, **bold**, or `code`.\n"};

static const char *const approval_details[] = {
    NULL,
    "Review the proposed changes before continuing.\n"
    "Check the public API and the tests.\n"
    "Report any remaining risks.",
    "Check the complete behavior before proceeding: confirm that ordinary "
    "user input stays responsive while output is streaming, that queued and "
    "steering messages keep their original destination, and that approval "
    "decisions remain in arrival order through editing and requeueing. "
    "Include the evidence from focused tests, describe any remaining "
    "uncertainty, and keep the explanation clear enough to review without "
    "opening the implementation. Paragraph complete.",
    "Verification plan:\n"
    "01. Check intent and constraints.\n"
    "02. Keep ordinary input responsive.\n"
    "03. Preserve FIFO approval order.\n"
    "04. Retain steering intent.\n"
    "05. Keep normal queue previews visible.\n"
    "06. Preserve the reasoning row.\n"
    "07. Wrap text to the terminal width.\n"
    "08. Limit previews to three lines.\n"
    "09. Mark omitted text with an ellipsis.\n"
    "10. Keep full messages editable.\n"
    "11. Retain holds after editing.\n"
    "12. Report verification evidence.\n"
    "End of verification plan."};
static int is_shell_command(const char *line) {
  return strcmp(line, "!sh") == 0 || strcmp(line, "/shell") == 0;
}

static int command_named(const char *line, const char *name) {
  size_t length = strlen(name);
  return !strncmp(line, name, length) &&
         (line[length] == '\0' || line[length] == ' ');
}

static int is_local_command(const char *line) {
  return !strcmp(line, "/status") || !strcmp(line, "/resume") ||
         command_named(line, "/approve") || command_named(line, "/stream") ||
         command_named(line, "/terminal-lock");
}

static unsigned int demo_stream_initial_delay(const char *value) {
  char *end;
  long milliseconds;
  if (!value || !*value)
    return DEMO_STREAM_DEFAULT_DELAY_US;
  errno = 0;
  milliseconds = strtol(value, &end, 10);
  if (errno || end == value || *end || milliseconds < 0 || milliseconds > 60000)
    return DEMO_STREAM_DEFAULT_DELAY_US;
  return (unsigned int)milliseconds * 1000u;
}

static unsigned int demo_stream_delay(enum demo_stream_mode mode,
                                      unsigned int initial_delay) {
  if (mode == DEMO_STREAM_DIRECT)
    return 0;
  if (mode == DEMO_STREAM_LLM && !initial_delay)
    return DEMO_STREAM_DEFAULT_DELAY_US;
  return initial_delay;
}

static enum demo_stream_mode demo_stream_setting(const char *line,
                                                 unsigned int delay) {
  if (!strcmp(line, "/stream"))
    return delay ? DEMO_STREAM_DIRECT : DEMO_STREAM_LLM;
  if (!strcmp(line, "/stream direct"))
    return DEMO_STREAM_DIRECT;
  if (!strcmp(line, "/stream llm"))
    return DEMO_STREAM_LLM;
  return DEMO_STREAM_INVALID;
}

static const char *demo_stream_message(enum demo_stream_mode mode) {
  if (mode == DEMO_STREAM_DIRECT)
    return "Streaming mode: direct.";
  if (mode == DEMO_STREAM_LLM)
    return "Streaming mode: LLM simulation.";
  return "Usage: /stream [direct|llm]";
}

static int terminal_lock_setting(const char *line, int enabled) {
  if (!strcmp(line, "/terminal-lock"))
    return !enabled;
  if (!strcmp(line, "/terminal-lock on"))
    return 1;
  if (!strcmp(line, "/terminal-lock off"))
    return 0;
  return -1;
}
#endif
