#ifndef SOFTLINE_CBOR_H
#define SOFTLINE_CBOR_H

#include <c89cbor/io.h>
#include <softline/softline.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Drain native encoder spans directly to a libcbor sink. Decrement budget by
 * accepted bytes. OUTPUT leaves the remaining span pinned for the next call.
 * Define SOFTLINE_CBOR_IMPLEMENTATION in one translation unit to use this
 * adapter, and link that unit with c89.systems' libcbor. */
int sl_cbor_drain(c89cbor_encoder *encoder, const c89cbor_sink *sink,
                  size_t *budget);

#ifdef SOFTLINE_CBOR_IMPLEMENTATION
int sl_cbor_drain(c89cbor_encoder *encoder, const c89cbor_sink *sink,
                  size_t *budget) {
  if (!encoder || !sink || !sink->write || !budget)
    return C89CBOR_INVALID;
  for (;;) {
    const void *bytes;
    size_t length, written = 0;
    int status = c89cbor_encoder_peek(encoder, &bytes, &length);
    if (status != C89CBOR_OUTPUT || !*budget)
      return status;
    if (length > *budget)
      length = *budget;
    status = sink->write(sink->context, bytes, length, &written);
    if (status < 0)
      return status;
    if (written > length || (status == C89CBOR_OK && !written) ||
        (status != C89CBOR_OK && status != C89CBOR_OUTPUT))
      return C89CBOR_IO;
    if (written && c89cbor_encoder_advance(encoder, written) < 0)
      return C89CBOR_IO;
    *budget -= written;
    if (status == C89CBOR_OUTPUT)
      return status;
  }
}
#endif

#ifdef __cplusplus
}
#endif
#endif
