#ifndef SOFTLINE_CBOR_H
#define SOFTLINE_CBOR_H

/** Optional libcbor integration for application-owned transports.
 *
 * A consumer that includes this header supplies libcbor. Use
 * `sl_queue_backend_pin_request()` inside the active request callback
 * to borrow its strings across partial writes. Pass the spans directly to
 * libcbor and release the lease with `sl_queue_backend_unpin_request()`. */
#include <c89cbor/io.h>
#include <softline/softline.h>

#endif
