#ifndef SOFTLINE_CBOR_H
#define SOFTLINE_CBOR_H

/* Optional libcbor integration. Pin the existing Softline request fields
 * with sl_queue_backend_pin_request and pass their spans to libcbor directly.
 * The application links libcbor and supplies its own transport. */
#include <c89cbor/io.h>
#include <softline/softline.h>

#endif
