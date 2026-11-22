#ifndef SOFTLINE_CBOR_H
#define SOFTLINE_CBOR_H

/** @file
 * Optional libcbor integration for application-owned transports.
 * A consumer that includes this header supplies libcbor. Use
 * sl_queue_backend_pin_request() to borrow request text across partial
 * writes and pass those spans directly to libcbor. */
#include <c89cbor/io.h>
#include <softline/softline.h>

#endif
