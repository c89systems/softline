# libvterm 0.3.3

Private VT screen engine from Paul Evans's upstream release.
Source: https://www.leonerd.org.uk/code/libvterm/libvterm-0.3.3.tar.gz
SHA-256: `09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0`

Softline builds the library sources with prefixed, hidden symbols.
Local parser changes reject oversized CSI parameters, bound the OSC command
number and avoid invalid string-length arithmetic on chunk boundaries.
The state engine also ignores repeat-character commands before it has a
character to repeat. The public Softline headers do not include libvterm.
