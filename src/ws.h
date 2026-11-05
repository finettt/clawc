/* clawc - ws.h: minimal WebSocket client (RFC 6455) - the Discord transport. */
#ifndef CLAWC_WS_H
#define CLAWC_WS_H

#include "util.h"
#include "tls.h"

typedef struct {
    int fd;
    TlsConn *tls;   /* NULL for ws:// */
    Buf acc;        /* fragmented message assembly */
} WsConn;

/* ws:// or wss:// URL. 0 = ok. */
int ws_connect(WsConn *c, const char *url, int insecure, char **err);
/* send one text message (masked, as clients must). 0 = ok */
int ws_send_text(WsConn *c, const char *s, size_t n);
/* receive one complete message (handles ping/pong + fragmentation).
   0 = ok, 1 = closed, -1 = error/timeout */
int ws_recv_msg(WsConn *c, Buf *out, int timeout_s);
void ws_close(WsConn *c);

#endif
