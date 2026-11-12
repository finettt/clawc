/* clawc - net.h: outbound TCP connect with optional proxy tunnelling.
 *
 * One place decides how the agent reaches the network, so every channel (LLM,
 * Telegram, Matrix, Discord REST + gateway, fetch) honours the same setting.
 * Proxy support is transport-only: TLS keeps verifying the ORIGIN hostname.
 */
#ifndef CLAWC_NET_H
#define CLAWC_NET_H

/* Configure once at startup, before any request is made.
 *   proxy    - NULL/"" for a direct connection; otherwise
 *              http[s]://[user:pass@]host:port  |  socks5[h]://[user:pass@]host:port
 *   no_proxy - comma-separated bypass list (exact host, .suffix, host:port)
 * Both strings are copied. Returns 0 on success, -1 if the URL cannot be parsed
 * (the caller reports and exits; a malformed proxy must not silently fall back
 * to a direct connection). */
int net_set_proxy(const char *proxy, const char *no_proxy);

/* Human-readable description of the active route, for `clawc config`/`doctor`:
 * NULL when connecting directly. */
const char *net_proxy_active(void);

/* Should host bypass the proxy? Exposed for tests. */
int net_bypasses(const char *host);

/* Connect to host:port, tunnelling through the proxy when one is active.
 * Returns a connected fd with the send/recv timeouts applied, or -1 with a
 * malloc'd message in *err (caller frees). *via, if non-NULL, is set to a
 * static string describing the route ("direct", "http-connect", "socks5"). */
int net_connect(const char *host, const char *port, int timeout_s,
                char **err, const char **via);

/* Base64 (standard alphabet, padded) for Proxy-Authorization. Malloc'd. */
char *net_b64(const char *in);

#endif
