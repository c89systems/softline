#ifndef SOFTLINE_CHAT_DEMO_DATA_H
#define SOFTLINE_CHAT_DEMO_DATA_H
#include <string.h>
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
         command_named(line, "/approve");
}
#endif
