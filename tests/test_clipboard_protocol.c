#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr,                                                          \
              "protocol assertion failed: %s (%s, sequence %u, fault %d)\n",   \
              #c, mode, sequence, (int)fault);                                 \
      goto cleanup;                                                            \
    }                                                                          \
  } while (0)

enum fault {
  NO_FAULT,
  MISSING_REPLY,
  PARTIAL_REPLY,
  MISSING_EVENT,
  MISSING_CHUNK_EVENT,
  MISSING_TERMINATOR_EVENT,
  MISSING_TERMINATOR_REPLY,
  FLOOD
};

static double now(void) {
  struct timespec value;
  clock_gettime(CLOCK_MONOTONIC, &value);
  return (double)value.tv_sec + (double)value.tv_nsec / 1000000000.0;
}

static void le16(unsigned char *p, unsigned value) {
  p[0] = (unsigned char)value;
  p[1] = (unsigned char)(value >> 8);
}
static void le32(unsigned char *p, size_t value) {
  le16(p, (unsigned)value);
  le16(p + 2, (unsigned)(value >> 16));
}
static uint32_t word32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
/* Return zero only for an ordinary boundary EOF, never a partial message. */
static int receive(int fd, unsigned char *data, size_t length) {
  size_t offset = 0;
  ssize_t count;
  while (offset < length) {
    count = read(fd, data + offset, length - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count == 0)
      return offset ? -1 : 0;
    if (count < 0 && errno == ECONNRESET && offset == 0)
      return 0;
    if (count < 0)
      return -1;
    offset += (size_t)count;
  }
  return 1;
}
static int send_all(int fd, const unsigned char *data, size_t length) {
  ssize_t count;
  while (length) {
    count = write(fd, data, length);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return -1;
    data += count;
    length -= (size_t)count;
  }
  return 0;
}

/* Keep valid but irrelevant packets flowing beyond the child's whole-operation
 * budget. A fresh timeout per packet would keep accepting these indefinitely.
 */
static void flood(int fd, const unsigned char *packet) {
  unsigned char batch[4096];
  size_t offset;
  double deadline = now() + 0.7;
  for (offset = 0; offset < sizeof(batch); offset += 32)
    memcpy(batch + offset, packet, 32);
  while (now() < deadline && send_all(fd, batch, sizeof(batch)) == 0) {
  }
}

static unsigned char *load(const char *path, size_t *length) {
  FILE *file = fopen(path, "rb");
  long size;
  unsigned char *bytes = NULL;
  if (!file)
    return NULL;
  if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < 0 ||
      fseek(file, 0, SEEK_SET))
    goto done;
  bytes = (unsigned char *)malloc((size_t)size + 1);
  if (!bytes)
    goto done;
  if (fread(bytes, 1, (size_t)size, file) != (size_t)size) {
    free(bytes);
    bytes = NULL;
  } else
    *length = (size_t)size;
done:
  fclose(file);
  return bytes;
}
static int probe(const char *fixture, const char *directory, const char *mode,
                 unsigned sequence, const unsigned char *payload, size_t length,
                 size_t chunk_size, int broken, unsigned wrapped_opcode,
                 int natural_wrap, enum fault fault) {
  int pair[2] = {-1, -1}, result = 1, status, received, initial = 1;
  int complete = 0, acknowledged = 0, is_direct = !strcmp(mode, "direct");
  pid_t child = -1;
  struct timeval timeout;
  unsigned char head[4], request[64], reply[32], size_bytes[4];
  unsigned char padding[3] = {0, 0, 0}, *actual = NULL;
  const unsigned char *data;
  size_t served = 0, count, actual_length = 0;
  unsigned requests = 0, opcode, serial, wraps = 0;
  double started = now(), elapsed;
  char fd_text[32], sequence_text[32], output[4096];
  snprintf(output, sizeof(output), "%s/image.png", directory);
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  timeout.tv_sec = fault ? 1 : 12;
  timeout.tv_usec = 0;
  CHECK(setsockopt(pair[1], SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout)) == 0);
  CHECK(setsockopt(pair[1], SOL_SOCKET, SO_SNDTIMEO, &timeout,
                   sizeof(timeout)) == 0);
  snprintf(fd_text, sizeof(fd_text), "%d", pair[0]);
  snprintf(sequence_text, sizeof(sequence_text), "%u", sequence);
  if (broken) {
    close(pair[1]);
    pair[1] = -1;
  }
  child = fork();
  CHECK(child >= 0);
  if (child == 0) {
    if (pair[1] >= 0)
      close(pair[1]);
    execl(fixture, fixture, mode, fd_text, sequence_text, output,
          fault ? "120" : "10000", (char *)NULL);
    _exit(127);
  }
  close(pair[0]);
  pair[0] = -1;
  while (!broken && (received = receive(pair[1], head, sizeof(head))) != 0) {
    CHECK(received == 1);
    count = ((unsigned)head[2] | (unsigned)head[3] << 8) * 4;
    CHECK(count >= 4 && count - 4 <= sizeof(request));
    if (count > 4)
      CHECK(receive(pair[1], request, count - 4) == 1);
    ++requests;
    serial = (sequence + requests) & 65535;
    opcode = head[0];
    if (serial == 0) {
      CHECK(opcode == wrapped_opcode);
      ++wraps;
    }
    memset(reply, 0, sizeof(reply));
    reply[0] = 1;
    le16(reply + 2, serial);
    if (opcode == 16) {
      le32(reply + 8, 30);
      if (fault == MISSING_REPLY)
        continue;
      if (fault == PARTIAL_REPLY) {
        CHECK(send_all(pair[1], reply, 16) == 0);
        continue;
      }
      if (fault == FLOOD) {
        le16(reply + 2, (serial + 1) & 65535);
        flood(pair[1], reply);
        continue;
      }
      CHECK(send_all(pair[1], reply, sizeof(reply)) == 0);
    } else if (opcode == 20) {
      CHECK(count == 24 && word32(request) == 10 && word32(request + 4) == 20);
      CHECK(word32(request + 12) == (is_direct ? served / 4 : 0));
      if (fault == MISSING_REPLY ||
          (fault == MISSING_TERMINATOR_REPLY && !initial && served == length))
        continue;
      if (fault == PARTIAL_REPLY) {
        CHECK(send_all(pair[1], reply, 16) == 0);
        continue;
      }
      if (initial && !is_direct) {
        le32(size_bytes, length);
        data = size_bytes;
        count = 4;
        reply[1] = 32;
        le32(reply + 8, 40);
        le32(reply + 16, 1);
        initial = 0;
      } else {
        count = length - served;
        if (count > chunk_size)
          count = chunk_size;
        data = payload + served;
        served += count;
        reply[1] = 8;
        le32(reply + 8, 30);
        le32(reply + 16, count);
        if (is_direct)
          le32(reply + 12, length - served);
        complete = is_direct ? served == length : count == 0;
      }
      le32(reply + 4, (count + 3) / 4);
      CHECK(send_all(pair[1], reply, sizeof(reply)) == 0);
      CHECK(send_all(pair[1], data, count) == 0);
      CHECK(send_all(pair[1], padding, (4 - count % 4) % 4) == 0);
    } else if (opcode == 18 || opcode == 19 || opcode == 24) {
      if (opcode == 19) {
        CHECK(count == 12 && word32(request) == 10 &&
              word32(request + 4) == 20);
      }
      if (opcode == 19 && complete) {
        acknowledged = 1;
        continue;
      }
      if (opcode == 19 && !strcmp(mode, "transfer")) {
        /* Our delete notification is not the owner's next NewValue event. */
        reply[0] = 28;
        le32(reply + 4, 10);
        le32(reply + 8, 20);
        reply[16] = 1;
        CHECK(send_all(pair[1], reply, sizeof(reply)) == 0);
        reply[16] = 0;
      }
      if ((fault == MISSING_EVENT && opcode != 19) ||
          (fault == FLOOD && opcode == 24) ||
          (fault == MISSING_EVENT && opcode == 19 &&
           !strcmp(mode, "transfer")) ||
          (fault == MISSING_CHUNK_EVENT && opcode == 19 && served > 0 &&
           served < length) ||
          (fault == MISSING_TERMINATOR_EVENT && opcode == 19 && !initial &&
           served == length))
        continue;
      reply[0] = (unsigned char)(opcode == 24 ? 31 : 28);
      if (opcode == 24) {
        le32(reply + 4, 123);
        le32(reply + 8, 10);
        le32(reply + 12, 25);
        le32(reply + 16, 30);
        le32(reply + 20, 20);
      } else {
        le32(reply + 4, 10);
        le32(reply + 8, 20);
        le32(reply + 12, 123);
      }
      if (fault == FLOOD && opcode != 24) {
        le32(reply + 4, 11);
        flood(pair[1], reply);
        continue;
      }
      received = send_all(pair[1], reply, sizeof(reply));
      CHECK(received == 0 ||
            (opcode == 19 && (errno == EPIPE || errno == ECONNRESET)));
    } else
      CHECK(opcode == 1);
  }
  CHECK(waitpid(child, &status, 0) == child);
  child = -1;
  elapsed = now() - started;
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == (broken || fault ? 1 : 0));
  if (fault) {
    if (elapsed < 0.08 || elapsed >= 0.65)
      fprintf(stderr, "Clipboard deadline elapsed: %.3f seconds\n", elapsed);
    CHECK(elapsed >= 0.08 && elapsed < 0.65);
    result = 0;
    goto cleanup;
  }
  if (!broken && wrapped_opcode)
    CHECK(wraps == 1);
  if (natural_wrap)
    CHECK(requests > 65536);
  if (!broken && length) {
    actual = load(output, &actual_length);
    CHECK(actual && actual_length == length &&
          memcmp(actual, payload, length) == 0);
    CHECK(served == length);
    CHECK(complete && acknowledged);
  }
  result = 0;
cleanup:
  if (child > 0) {
    kill(child, SIGTERM);
    waitpid(child, &status, 0);
  }
  if (pair[0] >= 0)
    close(pair[0]);
  if (pair[1] >= 0)
    close(pair[1]);
  free(actual);
  unlink(output);
  return result;
}
int main(int argc, char **argv) {
  const char *modes[] = {"atom", "window", "delete", "timestamp", "select"};
  const unsigned opcodes[] = {16, 1, 19, 18, 24};
  char directory[4096];
  size_t small_length = 0, large_length = 0, i;
  unsigned char *small = NULL, *large = NULL;
  int result = 1;
  if (argc != 5)
    return 2;
  signal(SIGPIPE, SIG_IGN);
  snprintf(directory, sizeof(directory), "%s/clipboard-protocol.XXXXXX",
           argv[2]);
  if (!mkdtemp(directory))
    return 1;
  small = load(argv[3], &small_length);
  large = load(argv[4], &large_length);
  if (!small || !large || large_length < 17 * 1024 * 1024)
    goto done;
  for (i = 0; i < 5; ++i) {
    unsigned sequence = i == 4 ? 65534 : 65535;
    if (probe(argv[1], directory, modes[i], sequence, NULL, 0, 512, 0,
              opcodes[i], 0, NO_FAULT) ||
        probe(argv[1], directory, modes[i], sequence, NULL, 0, 512, 1, 0, 0,
              NO_FAULT))
      goto done;
  }
  if (probe(argv[1], directory, "transfer", 65534, small, small_length, 512, 0,
            19, 0, NO_FAULT) ||
      probe(argv[1], directory, "transfer", 65535, small, small_length, 512, 0,
            20, 0, NO_FAULT) ||
      probe(argv[1], directory, "transfer", 0, large, large_length, 512, 0, 19,
            1, NO_FAULT) ||
      probe(argv[1], directory, "transfer", 0, large, large_length, 4096, 0, 0,
            0, NO_FAULT) ||
      probe(argv[1], directory, "direct", 0, small, small_length, 16, 0, 0, 0,
            NO_FAULT))
    goto done;
  for (i = MISSING_REPLY; i <= FLOOD; ++i) {
    if ((i == MISSING_REPLY || i == PARTIAL_REPLY || i == FLOOD) &&
        probe(argv[1], directory, "atom", 0, NULL, 0, 512, 0, 0, 0,
              (enum fault)i))
      goto done;
    if ((i == MISSING_EVENT || i == FLOOD) &&
        (probe(argv[1], directory, "timestamp", 0, NULL, 0, 512, 0, 0, 0,
               (enum fault)i) ||
         probe(argv[1], directory, "select", 0, NULL, 0, 512, 0, 0, 0,
               (enum fault)i)))
      goto done;
    if (probe(argv[1], directory, "transfer", 0, small, small_length, 16, 0, 0,
              0, (enum fault)i))
      goto done;
  }
  result = 0;
done:
  free(small);
  free(large);
  rmdir(directory);
  return result;
}
