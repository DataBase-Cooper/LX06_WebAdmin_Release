These unmodified public headers are from curl 7.55.1, the libcurl version in
the LX06 stock firmware. They are used only to compile `model-http.c`; the
helper dynamically loads the speaker's existing `libcurl.so.4` at runtime.
No curl implementation, TLS library or platform authentication is embedded.

Upstream: https://github.com/curl/curl/tree/curl-7_55_1/include/curl

Copyright and license: see [COPYING](COPYING) and each header's notices.
