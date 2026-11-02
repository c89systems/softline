#define _POSIX_C_SOURCE 200809L

#include "softline/softline.h"

#include <libmdf/mdf.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* The test composer never accumulates rendered output: libmdf pushes through
 * a bounded pipe and the owner thread forwards each read to Softline's live
 * output session. libsoftline itself has no libmdf dependency. */
struct markdown_source {
  const char *text;
  size_t length;
  size_t offset;
  size_t reads;
  size_t max_chunk;
};

struct renderer_stream {
  int write_fd;
  int osc8;
  struct markdown_source source;
  size_t feeds;
  size_t sink_writes;
  size_t first_sink_source_offset;
  mdf_status status;
};

struct softline_pull {
  int read_fd;
  char chunk[7];
  size_t reads;
  size_t bytes;
};

struct output_check {
  int read_fd;
  const char *expected;
  size_t expected_length;
  size_t offset;
  size_t reads;
  int failed;
};

static int write_all(int fd, const char *src, size_t length) {
  ssize_t written;

  while (length > 0) {
    written = write(fd, src, length);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (written == 0)
      return -1;
    src += written;
    length -= (size_t)written;
  }
  return 0;
}

static size_t read_markdown(void *userdata, char *dst, size_t capacity,
                            int *error) {
  struct markdown_source *source;
  size_t remaining;
  size_t length;

  source = (struct markdown_source *)userdata;
  *error = 0;
  if (source == NULL || dst == NULL || capacity == 0) {
    *error = 1;
    return 0;
  }
  if (source->offset == source->length)
    return 0;
  remaining = source->length - source->offset;
  length = source->max_chunk;
  if (length > capacity)
    length = capacity;
  if (length > remaining)
    length = remaining;
  memcpy(dst, source->text + source->offset, length);
  source->offset += length;
  source->reads++;
  return length;
}

static int write_rendered(void *userdata, const char *src, size_t length) {
  struct renderer_stream *stream;

  stream = (struct renderer_stream *)userdata;
  if (stream == NULL || src == NULL || length == 0)
    return -1;
  if (stream->first_sink_source_offset == (size_t)-1)
    stream->first_sink_source_offset = stream->source.offset;
  stream->sink_writes++;
  return write_all(stream->write_fd, src, length);
}

static void *render_markdown(void *userdata) {
  struct renderer_stream *stream;
  mdf_options options;
  mdf_sink sink;
  mdf *renderer;
  char chunk[2];
  size_t length;
  int error;

  stream = (struct renderer_stream *)userdata;
  renderer = NULL;
  mdf_options_init(&options);
  options.ansi_mode = MDF_ANSI_ON;
  options.osc8 = stream->osc8;
  options.boring = 1;
  options.width = 72;
  stream->status = mdf_create(MDF_FORMAT_ANSI, &options, &renderer);
  if (stream->status == MDF_OK) {
    sink.userdata = stream;
    sink.write = write_rendered;
    stream->status = renderer->set_sink(renderer, &sink);
  }
  if (stream->status == MDF_OK) {
    for (;;) {
      error = 0;
      length = read_markdown(&stream->source, chunk, sizeof(chunk), &error);
      if (error != 0) {
        stream->status = MDF_ERROR_IO;
        break;
      }
      if (length == 0)
        break;
      stream->status = renderer->feed(renderer, chunk, length);
      if (stream->status != MDF_OK)
        break;
      stream->feeds++;
      stream->status = renderer->flush(renderer);
      if (stream->status != MDF_OK)
        break;
    }
    if (stream->status == MDF_OK)
      stream->status = renderer->finish_document(renderer);
  }
  if (renderer != NULL)
    renderer->destroy(renderer);
  (void)close(stream->write_fd);
  return NULL;
}

static int pull_rendered(sl_t *self, void *userdata, const char **chunk,
                         size_t *length) {
  struct softline_pull *stream;
  ssize_t read_count;

  (void)self;
  stream = (struct softline_pull *)userdata;
  if (stream == NULL || chunk == NULL || length == NULL)
    return SL_ERROR_INVALID;
  do {
    read_count = read(stream->read_fd, stream->chunk, sizeof(stream->chunk));
  } while (read_count < 0 && errno == EINTR);
  if (read_count < 0)
    return SL_ERROR_IO;
  if (read_count == 0) {
    *chunk = NULL;
    *length = 0;
    return SL_OK;
  }
  stream->reads++;
  stream->bytes += (size_t)read_count;
  *chunk = stream->chunk;
  *length = (size_t)read_count;
  return SL_OK;
}

static void *check_output(void *userdata) {
  struct output_check *check;
  char buffer[11];
  ssize_t read_count;

  check = (struct output_check *)userdata;
  for (;;) {
    do {
      read_count = read(check->read_fd, buffer, sizeof(buffer));
    } while (read_count < 0 && errno == EINTR);
    if (read_count < 0) {
      check->failed = 1;
      break;
    }
    if (read_count == 0)
      break;
    check->reads++;
    if ((size_t)read_count > check->expected_length - check->offset ||
        memcmp(check->expected + check->offset, buffer, (size_t)read_count) !=
            0) {
      fprintf(stderr, "libmdf streaming test: unexpected terminal bytes: ");
      (void)fwrite(buffer, 1, (size_t)read_count, stderr);
      fputc('\n', stderr);
      check->failed = 1;
    }
    if ((size_t)read_count > check->expected_length - check->offset)
      check->offset = check->expected_length;
    else
      check->offset += (size_t)read_count;
  }
  if (check->offset != check->expected_length)
    check->failed = 1;
  return NULL;
}

struct geometry_output {
  char bytes[128];
  size_t length;
  size_t writes;
};

static int capture_geometry(void *userdata, const char *bytes, size_t length) {
  struct geometry_output *output;
  output = (struct geometry_output *)userdata;
  if (!output || !bytes || length > sizeof(output->bytes) - output->length)
    return -1;
  memcpy(output->bytes + output->length, bytes, length);
  output->length += length;
  output->writes++;
  return 0;
}

static int check_live_geometry(void) {
  static const char first[] = "  alpha";
  static const char expected[] = "  alpha\n\n    beta\n";
  struct geometry_output output;
  mdf_options options;
  mdf_sink sink;
  mdf *renderer;
  mdf_status status;
  size_t before_length;
  size_t before_writes;
  int stage;

  memset(&output, 0, sizeof(output));
  mdf_options_init(&options);
  options.ansi_mode = MDF_ANSI_ON;
  options.boring = 1;
  options.width = 40;
  options.margin_left = 2;
  renderer = NULL;
  stage = 0;
  status = mdf_create(MDF_FORMAT_ANSI, &options, &renderer);
  if (status != MDF_OK)
    goto failed;
  sink.userdata = &output;
  sink.write = capture_geometry;
  status = renderer->set_sink(renderer, &sink);
  if (status != MDF_OK)
    goto failed;
  status = renderer->feed(renderer, "alpha\n\n", 7);
  if (status != MDF_OK)
    goto failed;
  status = renderer->flush(renderer);
  stage = 1;
  if (status != MDF_OK || output.length != sizeof(first) - 1 ||
      memcmp(output.bytes, first, sizeof(first) - 1) != 0)
    goto failed;
  before_length = output.length;
  before_writes = output.writes;
  status = renderer->set_geometry(renderer, 40, 4, 1);
  stage = 2;
  if (status != MDF_OK || output.length != before_length ||
      output.writes != before_writes)
    goto failed;
  status = renderer->feed(renderer, "beta\n", 5);
  if (status != MDF_OK)
    goto failed;
  status = renderer->finish_document(renderer);
  stage = 3;
  if (status != MDF_OK || output.length != sizeof(expected) - 1 ||
      memcmp(output.bytes, expected, sizeof(expected) - 1) != 0)
    goto failed;
  renderer->destroy(renderer);
  return 0;

failed:
  fprintf(stderr,
          "libmdf geometry test: stage=%d status=%d length=%lu output=", stage,
          (int)status, (unsigned long)output.length);
  (void)fwrite(output.bytes, 1, output.length, stderr);
  fputc('\n', stderr);
  if (renderer)
    renderer->destroy(renderer);
  return 1;
}

int main(int argc, char **argv) {
  const char *markdown = argc > 1
                             ? "before [label](https://example.org/) after\n"
                             : "# hello\n\nworld\n";
  const char *expected =
      argc > 1 ? "before \033]8;;https://example.org/\033\\label\033]8;;\033\\ "
                 "after\n"
               : "# hello\n\nworld\n";
  int rendered_pipe[2];
  int output_pipe[2];
  pthread_t renderer_thread;
  pthread_t output_thread;
  struct renderer_stream renderer;
  struct softline_pull pull;
  struct output_check output;
  sl_config_t config;
  sl_osc8_info_t info;
  sl_t *softline;
  int status;
  int renderer_started;
  int output_started;

  if (check_live_geometry() != 0)
    return 1;
  memset(&renderer, 0, sizeof(renderer));
  memset(&pull, 0, sizeof(pull));
  memset(&output, 0, sizeof(output));
  renderer_started = 0;
  output_started = 0;
  if (pipe(rendered_pipe) != 0) {
    fprintf(stderr, "libmdf streaming test: pipe setup failed\n");
    return 1;
  }
  if (pipe(output_pipe) != 0) {
    (void)close(rendered_pipe[0]);
    (void)close(rendered_pipe[1]);
    fprintf(stderr, "libmdf streaming test: pipe setup failed\n");
    return 1;
  }
  renderer.write_fd = rendered_pipe[1];
  renderer.source.text = markdown;
  renderer.source.length = strlen(markdown);
  renderer.source.max_chunk = 2;
  renderer.first_sink_source_offset = (size_t)-1;
  pull.read_fd = rendered_pipe[0];
  output.read_fd = output_pipe[0];
  output.expected = expected;
  output.expected_length = strlen(expected);
  sl_config_init(&config);
  config.output_fd = output_pipe[1];
  config.osc8_mode = argc > 1 ? SL_OSC8_ON : SL_OSC8_OFF;
  softline = sl_create_with_config(&config);
  if (softline == NULL) {
    (void)close(rendered_pipe[0]);
    (void)close(rendered_pipe[1]);
    (void)close(output_pipe[0]);
    (void)close(output_pipe[1]);
    fprintf(stderr, "libmdf streaming test: softline creation failed\n");
    return 1;
  }
  if (sl_get_osc8_info(softline, &info) != SL_OK)
    return 1;
  renderer.osc8 = info.enabled;
  if (pthread_create(&output_thread, NULL, check_output, &output) != 0) {
    softline->destroy(softline);
    (void)close(rendered_pipe[0]);
    (void)close(rendered_pipe[1]);
    (void)close(output_pipe[0]);
    (void)close(output_pipe[1]);
    fprintf(stderr, "libmdf streaming test: thread setup failed\n");
    return 1;
  }
  output_started = 1;
  if (pthread_create(&renderer_thread, NULL, render_markdown, &renderer) != 0) {
    softline->destroy(softline);
    (void)close(rendered_pipe[0]);
    (void)close(rendered_pipe[1]);
    (void)close(output_pipe[1]);
    (void)pthread_join(output_thread, NULL);
    (void)close(output_pipe[0]);
    fprintf(stderr, "libmdf streaming test: thread setup failed\n");
    return 1;
  }
  renderer_started = 1;
  status = sl_output_stream_begin(softline);
  while (status == SL_OK) {
    const char *chunk;
    size_t length;
    status = pull_rendered(softline, &pull, &chunk, &length);
    if (status != SL_OK || length == 0)
      break;
    status = sl_output_stream_write(softline, chunk, length);
  }
  if (status == SL_OK)
    status = sl_output_stream_end(softline);
  softline->destroy(softline);
  (void)close(output_pipe[1]);
  if (renderer_started)
    (void)pthread_join(renderer_thread, NULL);
  if (output_started)
    (void)pthread_join(output_thread, NULL);
  (void)close(rendered_pipe[0]);
  (void)close(output_pipe[0]);
  if (status != SL_OK || renderer.status != MDF_OK || output.failed ||
      renderer.source.reads < 2 || renderer.feeds < 2 ||
      renderer.sink_writes < 2 || pull.reads < 2 || output.reads < 2 ||
      renderer.first_sink_source_offset >= renderer.source.length) {
    fprintf(stderr,
            "libmdf streaming test: bridge failed (sl=%d mdf=%d output=%d "
            "source_reads=%lu feeds=%lu sink_writes=%lu pull_reads=%lu "
            "output_reads=%lu "
            "first_sink=%lu source_length=%lu)\n",
            status, (int)renderer.status, output.failed,
            (unsigned long)renderer.source.reads, (unsigned long)renderer.feeds,
            (unsigned long)renderer.sink_writes, (unsigned long)pull.reads,
            (unsigned long)output.reads,
            (unsigned long)renderer.first_sink_source_offset,
            (unsigned long)renderer.source.length);
    return 1;
  }
  return 0;
}
