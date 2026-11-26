#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#include "softline_history.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct sl_history_store {
  char *path;
  char *lock_path;
  size_t line_max;
};

/* SHA-256 is used solely for stable filenames, not authentication/encryption.
 * Keep the key mapping independent of release versions. */
static uint32_t sl_rotr(uint32_t x, unsigned int n) {
  return (x >> n) | (x << (32 - n));
}

static void sl_sha_block(uint32_t h[8], const unsigned char block[64]) {
  static const uint32_t k[64] = {
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
      0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
      0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
      0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
      0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
      0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
      0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
      0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
      0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
      0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
      0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
      0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
      0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
  uint32_t w[64], a, b, c, d, e, f, g, j, t1, t2;
  int i;
  for (i = 0; i < 16; i++)
    w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
           ((uint32_t)block[i * 4 + 2] << 8) | block[i * 4 + 3];
  for (i = 16; i < 64; i++)
    w[i] = w[i - 16] +
           (sl_rotr(w[i - 15], 7) ^ sl_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3)) +
           w[i - 7] +
           (sl_rotr(w[i - 2], 17) ^ sl_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10));
  a = h[0];
  b = h[1];
  c = h[2];
  d = h[3];
  e = h[4];
  f = h[5];
  g = h[6];
  j = h[7];
  for (i = 0; i < 64; i++) {
    t1 = j + (sl_rotr(e, 6) ^ sl_rotr(e, 11) ^ sl_rotr(e, 25)) +
         ((e & f) ^ (~e & g)) + k[i] + w[i];
    t2 = (sl_rotr(a, 2) ^ sl_rotr(a, 13) ^ sl_rotr(a, 22)) +
         ((a & b) ^ (a & c) ^ (b & c));
    j = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += j;
}

static void sl_history_hash(const char *key, char hex[65]) {
  static const char digits[] = "0123456789abcdef";
  uint32_t h[8] = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                   0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  unsigned char block[64];
  size_t len, n, i;
  uint32_t high, low;
  const unsigned char *p;
  len = strlen(key);
  high = (uint32_t)(len / 536870912U);
  low = (uint32_t)len << 3;
  p = (const unsigned char *)key;
  n = len;
  while (n >= 64) {
    sl_sha_block(h, p);
    p += 64;
    n -= 64;
  }
  memset(block, 0, sizeof(block));
  memcpy(block, p, n);
  block[n] = 0x80;
  if (n >= 56) {
    sl_sha_block(h, block);
    memset(block, 0, sizeof(block));
  }
  for (i = 0; i < 4; i++) {
    block[56 + i] = (unsigned char)(high >> (24 - i * 8));
    block[60 + i] = (unsigned char)(low >> (24 - i * 8));
  }
  sl_sha_block(h, block);
  for (i = 0; i < 32; i++) {
    unsigned int byte;
    byte = (unsigned int)((h[i / 4] >> (24 - (i % 4) * 8)) & 255);
    hex[i * 2] = digits[byte >> 4];
    hex[i * 2 + 1] = digits[byte & 15];
  }
  hex[64] = '\0';
}

static char *sl_path_join(const char *a, const char *b) {
  size_t n, m;
  char *path;
  n = strlen(a);
  m = strlen(b);
  if (n > (size_t)-1 - m - 2)
    return NULL;
  path = (char *)malloc(n + m + 2);
  if (path) {
    memcpy(path, a, n);
    path[n] = '/';
    memcpy(path + n + 1, b, m + 1);
  }
  return path;
}

static int sl_history_mkdir(char *path) {
  struct stat st;
  char *p;
  size_t n;
  n = strlen(path);
  while (n > 1 && path[n - 1] == '/')
    path[--n] = '\0';
  for (p = path + 1;; p++) {
    if (*p == '/' || *p == '\0') {
      char saved;
      int created;
      saved = *p;
      *p = '\0';
      created = mkdir(path, 0700) == 0;
      if ((!created && errno != EEXIST) ||
          (created && chmod(path, 0700) != 0)) {
        *p = saved;
        return SL_ERROR_IO;
      }
      *p = saved;
      if (!saved)
        break;
    }
  }
  /* Do not chmod existing parent directories. The store itself must be a
   * private directory, not a symlink or another user's directory. */
  if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid())
    return SL_ERROR_IO;
  if ((st.st_mode & 0777) != 0700 && chmod(path, 0700) != 0)
    return SL_ERROR_IO;
  return SL_OK;
}

int sl_history_store_create(const char *key, const char *directory,
                            size_t line_max, sl_history_store_t **out) {
  const char *base, *home;
  char *dir, *fallback;
  char hash[65], name[73];
  sl_history_store_t *store;
  int status;
  *out = NULL;
  if (!key || !*key || (directory && directory[0] != '/'))
    return SL_ERROR_INVALID;
  fallback = NULL;
  if (!directory) {
    base = getenv("XDG_STATE_HOME");
    if (!base || base[0] != '/') {
      home = getenv("HOME");
      if (!home || home[0] != '/')
        return SL_ERROR_INVALID;
      fallback = sl_path_join(home, ".local/state");
      if (!fallback)
        return SL_ERROR_NOMEM;
      base = fallback;
    }
    dir = sl_path_join(base, "softline/history");
  } else {
    dir = sl_path_join(directory, "");
  }
  free(fallback);
  if (!dir)
    return SL_ERROR_NOMEM;
  status = sl_history_mkdir(dir);
  if (status != SL_OK) {
    free(dir);
    return status;
  }
  store = (sl_history_store_t *)calloc(1, sizeof(*store));
  if (!store) {
    free(dir);
    return SL_ERROR_NOMEM;
  }
  sl_history_hash(key, hash);
  memcpy(name, hash, 64);
  memcpy(name + 64, ".history", 9);
  store->path = sl_path_join(dir, name);
  free(dir);
  if (store->path) {
    store->lock_path = (char *)malloc(strlen(store->path) + 6);
    if (store->lock_path)
      sprintf(store->lock_path, "%s.lock", store->path);
  }
  if (!store->path || !store->lock_path) {
    sl_history_store_destroy(store);
    return SL_ERROR_NOMEM;
  }
  store->line_max = line_max;
  *out = store;
  return SL_OK;
}

void sl_history_store_destroy(sl_history_store_t *store) {
  if (store) {
    free(store->path);
    free(store->lock_path);
    free(store);
  }
}

static int sl_history_fd(const char *path, int flags) {
  struct stat st;
  int fd;
  fd = open(path, flags | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
  if (fd < 0)
    return -1;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
      st.st_nlink != 1 || fchmod(fd, 0600) != 0) {
    close(fd);
    errno = EACCES;
    return -1;
  }
  return fd;
}

static int sl_history_lock(sl_history_store_t *store) {
  int fd, status;
  fd = sl_history_fd(store->lock_path, O_RDWR | O_CREAT);
  if (fd < 0)
    return -1;
  /* Lock the stable sidecar, not the history inode replaced by compaction.
   * flock locks independent opens independently, including within one process.
   * POSIX record locks would let separate owner-thread handles overlap.
   * See https://man7.org/linux/man-pages/man2/flock.2.html . */
  do {
    status = flock(fd, LOCK_EX);
  } while (status < 0 && errno == EINTR);
  if (status < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int sl_history_escape(unsigned char byte) {
  switch (byte) {
  case '\n':
    return 'n';
  case '\r':
    return 'r';
  case '\t':
    return 't';
  case '\\':
    return '\\';
  default:
    return 0;
  }
}

int sl_history_write_record(FILE *fp, const char *line) {
  const unsigned char *p;
  for (p = (const unsigned char *)line; *p; p++) {
    int escape;
    escape = sl_history_escape(*p);
    if (escape) {
      if (fputc('\\', fp) == EOF || fputc(escape, fp) == EOF)
        return SL_ERROR_IO;
    } else if (fputc(*p, fp) == EOF) {
      return SL_ERROR_IO;
    }
  }
  return fputc('\n', fp) == EOF ? SL_ERROR_IO : SL_OK;
}

int sl_history_read_record(FILE *fp, char *line, size_t cap, int terminated) {
  size_t n;
  int ch, bad, ended;
  n = 0;
  bad = 0;
  ended = 0;
  while ((ch = fgetc(fp)) != EOF) {
    if (ch == '\n') {
      ended = 1;
      break;
    }
    if (ch == '\\') {
      ch = fgetc(fp);
      if (ch == '\n' || (!terminated && ch == '\r')) {
        /* A physical record delimiter cannot be quoted by a trailing
         * backslash. Leave the delimiter for the next loop iteration. */
        ungetc(ch, fp);
        ch = '\\';
      } else if (ch == EOF)
        ch = '\\';
      else if (ch == 'n')
        ch = '\n';
      else if (ch == 'r')
        ch = '\r';
      else if (ch == 't')
        ch = '\t';
      else if (ch != '\\') {
        if (n < cap - 1)
          line[n++] = '\\';
        else
          bad = 1;
      }
    } else if (!terminated && ch == '\r') {
      ch = fgetc(fp);
      if (ch == '\n') {
        ended = 1;
        break;
      }
      if (ch != EOF)
        ungetc(ch, fp);
      ch = '\n';
    }
    if (ch == 0)
      bad = 1;
    if (n < cap - 1)
      line[n++] = (char)ch;
    else
      bad = 1;
  }
  line[n] = '\0';
  if (ferror(fp))
    return SL_ERROR_IO;
  if (!ended && (terminated || !n))
    return 0;
  return bad ? SL_ERROR_INVALID : 1;
}

static int sl_history_load_locked(sl_history_store_t *store,
                                  sl_history_emit_t emit, void *context) {
  int fd, status, record;
  FILE *fp;
  char *line;
  fd = sl_history_fd(store->path, O_RDONLY);
  if (fd < 0)
    return errno == ENOENT ? SL_OK : SL_ERROR_IO;
  fp = fdopen(fd, "r");
  if (!fp) {
    close(fd);
    return SL_ERROR_IO;
  }
  line = (char *)malloc(store->line_max + 1);
  if (!line) {
    fclose(fp);
    return SL_ERROR_NOMEM;
  }
  status = SL_OK;
  while ((record = sl_history_read_record(fp, line, store->line_max + 1, 1)) >
         0) {
    status = emit(context, line);
    if (status != SL_OK)
      break;
  }
  if (record < 0)
    status = record;
  free(line);
  if (fclose(fp) != 0 && status == SL_OK)
    status = SL_ERROR_IO;
  return status;
}

int sl_history_store_load(const char *key, sl_history_emit_t emit,
                          void *context, void *userdata) {
  sl_history_store_t *store;
  int lock, status;
  (void)key;
  store = (sl_history_store_t *)userdata;
  lock = sl_history_lock(store);
  if (lock < 0)
    return SL_ERROR_IO;
  status = sl_history_load_locked(store, emit, context);
  close(lock);
  return status;
}

/* Find the physical LF commit boundary from the end without retaining file
 * contents. Repair under the same lock before appending, never concatenate a
 * torn record with the next prompt. */
static int sl_history_repair(int fd, off_t *end) {
  struct stat st;
  unsigned char bytes[4096];
  off_t pos, start;
  size_t count;
  ssize_t amount, i;
  if (fstat(fd, &st) != 0)
    return SL_ERROR_IO;
  pos = st.st_size;
  while (pos > 0) {
    count = pos > (off_t)sizeof(bytes) ? sizeof(bytes) : (size_t)pos;
    start = pos - (off_t)count;
    do {
      amount = pread(fd, bytes, count, start);
    } while (amount < 0 && errno == EINTR);
    if (amount != (ssize_t)count)
      return SL_ERROR_IO;
    for (i = amount; i > 0; i--) {
      if (bytes[i - 1] == '\n') {
        *end = start + i;
        return ftruncate(fd, *end) == 0 ? SL_OK : SL_ERROR_IO;
      }
    }
    pos = start;
  }
  *end = 0;
  return ftruncate(fd, 0) == 0 ? SL_OK : SL_ERROR_IO;
}

int sl_history_store_append(const char *key, const char *prompt,
                            void *userdata) {
  sl_history_store_t *store;
  int lock, fd, status, escape;
  off_t end;
  size_t n, used, sent;
  ssize_t amount;
  unsigned char *record;
  const unsigned char *p;
  (void)key;
  store = (sl_history_store_t *)userdata;
  n = strlen(prompt);
  if (n > ((size_t)-1 - 1) / 2)
    return SL_ERROR_NOMEM;
  /* At most twice line_max bytes plus one LF, never a buffered history file. */
  record = (unsigned char *)malloc(n * 2 + 1);
  if (!record)
    return SL_ERROR_NOMEM;
  used = 0;
  for (p = (const unsigned char *)prompt; *p; p++) {
    escape = sl_history_escape(*p);
    if (escape)
      record[used++] = '\\';
    record[used++] = (unsigned char)(escape ? escape : *p);
  }
  record[used++] = '\n';
  lock = sl_history_lock(store);
  if (lock < 0) {
    free(record);
    return SL_ERROR_IO;
  }
  fd = sl_history_fd(store->path, O_RDWR | O_CREAT);
  status = SL_ERROR_IO;
  if (fd >= 0) {
    if (sl_history_repair(fd, &end) == SL_OK && lseek(fd, end, SEEK_SET) >= 0) {
      sent = 0;
      while (sent < used) {
        amount = write(fd, record + sent, used - sent);
        if (amount < 0 && errno == EINTR)
          continue;
        if (amount <= 0)
          break;
        sent += (size_t)amount;
      }
      status = sent == used ? SL_OK : SL_ERROR_IO;
      if (status != SL_OK)
        (void)ftruncate(fd, end);
    }
    if (close(fd) != 0)
      status = SL_ERROR_IO;
  }
  close(lock);
  free(record);
  return status;
}

struct sl_compact_entries {
  char **items;
  int cap, len, next;
};

static int sl_compact_emit(void *context, const char *line) {
  struct sl_compact_entries *entries;
  char *copy;
  entries = (struct sl_compact_entries *)context;
  if (!entries->cap || !*line)
    return SL_OK;
  copy = (char *)malloc(strlen(line) + 1);
  if (!copy)
    return SL_ERROR_NOMEM;
  strcpy(copy, line);
  free(entries->items[entries->next]);
  entries->items[entries->next] = copy;
  entries->next = (entries->next + 1) % entries->cap;
  if (entries->len < entries->cap)
    entries->len++;
  return SL_OK;
}

int sl_history_store_compact(sl_history_store_t *store, int max_entries) {
  struct sl_compact_entries entries;
  int lock, status, fd, i, index;
  char *temp;
  FILE *fp;
  memset(&entries, 0, sizeof(entries));
  entries.cap = max_entries;
  if (max_entries) {
    entries.items = (char **)calloc((size_t)max_entries, sizeof(char *));
    if (!entries.items)
      return SL_ERROR_NOMEM;
  }
  lock = sl_history_lock(store);
  if (lock < 0) {
    free(entries.items);
    return SL_ERROR_IO;
  }
  status = sl_history_load_locked(store, sl_compact_emit, &entries);
  temp = NULL;
  if (status == SL_OK) {
    temp = (char *)malloc(strlen(store->path) + 12);
    if (!temp)
      status = SL_ERROR_NOMEM;
  }
  if (status == SL_OK) {
    sprintf(temp, "%s.tmpXXXXXX", store->path);
    fd = mkstemp(temp);
    if (fd < 0) {
      status = SL_ERROR_IO;
    } else {
      if (fchmod(fd, 0600) != 0) {
        close(fd);
        fd = -1;
      }
      fp = fd >= 0 ? fdopen(fd, "w") : NULL;
      if (!fp) {
        if (fd >= 0)
          close(fd);
        status = SL_ERROR_IO;
      } else {
        for (i = 0; i < entries.len && status == SL_OK; i++) {
          index =
              entries.len == entries.cap ? (entries.next + i) % entries.cap : i;
          status = sl_history_write_record(fp, entries.items[index]);
        }
        if (fclose(fp) != 0)
          status = SL_ERROR_IO;
        if (status == SL_OK && rename(temp, store->path) != 0)
          status = SL_ERROR_IO;
      }
    }
    if (status != SL_OK)
      (void)unlink(temp);
  }
  free(temp);
  for (i = 0; i < entries.len; i++)
    free(entries.items[i]);
  free(entries.items);
  close(lock);
  return status;
}
