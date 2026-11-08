# Private X11 resolver

This is the Linux CMake/library/header subset of c-ares 1.34.9 (MIT), with
a BSD-3-Clause address-sorting helper. Both license notices ship in the SDK.
The unused APSL Apple header is excluded; this resolver is built only on Linux.
`provenance.json` records the release URL, verified archive SHA-256, and retained
file checksums. All retained upstream files are unchanged except the two
service helpers in
`softline.patch`: unused named-service lookup returns no service, and reverse
service formatting is numeric only. This avoids libc NSS references. Softline
always requests numeric X11 ports.

Softline reuses upstream configuration checks and the library source list.
It embeds the objects directly in its Linux static and shared libraries.
`scripts/cares-namespace.py` generates a private symbol namespace in the build
directory. Resolver headers and targets are not part of the public SDK.

Each paste uses an independent channel without event threads. Literal IPv4 and
IPv6 addresses, `/etc/hosts`, and DNS through `/etc/resolv.conf` (including search
domains, A/AAAA answers and UDP-to-TCP fallback) are handled by c-ares.
IPv6 interface qualifiers are retained through the kernel interface index.
Resolver socket processing shares the existing clipboard deadline. There is no libc
NSS, dynamic resolver/module loading, or additional link library.

To update: verify a new upstream release archive, retain the same subset,
reapply/reconsider `softline.patch`, regenerate the provenance checksums, and run
the resolver, clipboard, namespace and fully static consumer tests on GNU and
musl. Keep this notice, upstream license/authorship, patch and provenance in SDKs.

GCC 15 at `-O3` diagnoses the upstream generic DNS-record union accessor in
`ares_dns_rr_set_addr6`. Its key/type guards select the full IPv6 member; DNS
IPv6 and malformed-response tests also run under ASan/UBSan. This upstream
diagnostic is outside Softline's warning gate. Owned adapters and consumer
links remain subject to warnings as errors, including fully static links.
