#!/usr/bin/env python3
"""Mock LLM + Telegram API for clawc end-to-end tests.

Serves the same handlers over plain HTTP and HTTPS (self-signed cert):
  POST /v1/chat/completions        OpenAI-compatible LLM
  POST /v1/messages                Anthropic LLM
  POST /bot<TOKEN>/<method>        Telegram (getUpdates/sendMessage/sendChatAction/editMessageText)
  GET  /page                       plain web page (for the fetch_url tool)

LLM behavior is driven by the incoming request content (stateless):
  - tool result present in request -> text "FINAL: <tool result>"
  - prompt mentions the risky cmd  -> tool run_shell `rm -rf /tmp/clawc-risky-test`
  - prompt contains "HEARTBEAT"    -> text "HEARTBEAT_OK"
  - prompt contains "use-remember" -> tool remember
  - prompt contains "use-sandbox-write" -> tool write_file outside sandbox
  - prompt contains "use-fetch"    -> tool fetch_url (its own /page URL)
  - prompt contains "skill-test"   -> tool load_skill (anthropic path)
  - otherwise                      -> tool run_shell `echo hello-from-tool`

Telegram getUpdates is scripted statelessly via the client's offset and token:
  TESTTOKEN     offset 0 -> "hello telegram" (chat 42)
  CONFIRMTOKEN  offset 0 -> "run rm -rf /tmp/clawc-risky-test", offset 101 -> "/yes"
  STATUSTOKEN   offset 0 -> "/status"

Every request body is saved to /tmp/clawc-test-reqs/ for assertions.
"""
import json
import os
import re
import ssl
import sys
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
import socket, threading, base64, hashlib

RISKY = "/tmp/clawc-risky-test"
CANARY = "/home/user/landlock-canary"
RAW_ESCAPE = "/tmp/clawc-rawsys-escape"
WORKSPACE = "/tmp/clawc-test-ws"
REQDIR = "/tmp/clawc-test-reqs"
_counter = [0]
SELF_PORT = [8873]  # own http port, for fetch_url test


def save_request(body_bytes):
    os.makedirs(REQDIR, exist_ok=True)
    _counter[0] += 1
    with open(os.path.join(REQDIR, "req-%d.json" % _counter[0]), "wb") as f:
        f.write(body_bytes)


def first_user_text(msgs):
    """Most recent user message text (the current turn). Routing follows the
    current turn so stale history from earlier turns cannot derail it."""
    out = ""
    for m in msgs:
        if m.get("role") == "user":
            c = m.get("content")
            if isinstance(c, str):
                out = c
    return out


def tool_results_openai(msgs):
    """Tool results from the CURRENT turn only (after the last user message)."""
    out = []
    for m in reversed(msgs):
        if m.get("role") == "user":
            break
        if m.get("role") == "tool":
            out.append(m.get("content") or "")
    return out


def tool_results_anthropic(msgs):
    out = []
    for m in reversed(msgs):
        if m.get("role") == "user":
            c = m.get("content")
            if isinstance(c, str):
                break
        if m.get("role") == "user" and isinstance(m.get("content"), list):
            for blk in reversed(m["content"]):
                if isinstance(blk, dict) and blk.get("type") == "tool_result":
                    out.append(blk.get("content") or "")
            break
    return out


def llm_tool_for(ut):
    """Map user text to the tool call the mock LLM should emit."""
    if f"rm -rf {RISKY}" in ut:
        return "run_shell", {"command": f"rm -rf {RISKY}"}
    if "use-remember" in ut:
        return "remember", {"fact": "pizza is the favorite food"}
    if "use-jail-rawsys" in ut:
        return "run_shell", {"command": f"{RAW_ESCAPE} {CANARY}"}
    if "use-sandbox-symlink-out" in ut:
        return "write_file", {"path": "link-out/evil", "content": "x"}
    if "use-sandbox-symlink-in" in ut:
        return "write_file", {"path": "link-in", "content": "x"}
    if "use-sandbox-write" in ut:
        return "write_file", {"path": "/etc/evil", "content": "x"}
    if "use-fetch" in ut:
        return "fetch_url", {"url": "http://127.0.0.1:%d/page" % SELF_PORT[0]}
    if "use-listdir" in ut:
        return "list_dir", {}
    if "use-par-subagents" in ut:
        return "subagent", {"tasks": [{"task": "plain pong task", "tools": ["run_shell"]},
                                      {"task": "plain pong task", "tools": ["run_shell"]}]}
    if "use-nested-subagent" in ut:
        return "subagent", {"task": "try nesting"}
    if "try nesting" in ut:
        return "subagent", {"task": "double nested"}
    if "use-a-subagent" in ut:
        return "subagent", {"task": "plain pong task", "tools": ["run_shell"]}
    if "plain pong" in ut:
        return "run_shell", {"command": "echo pong-via-sub"}
    if "use-a2a-bad" in ut:
        return "a2a", {"peer": "127.0.0.1:1", "task": "x"}
    if "use-a2a" in ut:
        return "a2a", {"peer": "buddy", "task": "say the word mango"}
    if "Learning pass" in ut:
        return "write_skill", {"name": "autoskill",
                              "description": "auto-extracted procedure",
                              "content": "Step one. Verify with a smoke test."}
    if "use-search-history" in ut:
        return "search_memory", {"query": "hello telegram"}
    if "use-skill-nudge" in ut:
        return "load_skill", {"name": "greet"}
    if "use-goal-add" in ut:
        return "goal_add", {"task": "learn the ukulele", "due": "friday"}
    if "use-goal-done" in ut:
        return "goal_done", {"id": "g1"}
    if "Autonomous work cycle" in ut:
        return "goal_add", {"task": "made autonomous progress"}
    if "use-reflect" in ut:
        return "reflect", {"lesson": "the pizza protocol: verify toppings twice"}
    if "use-write-skill-bad" in ut:
        return "write_skill", {"name": "../evil", "description": "x", "content": "x"}
    if "use-write-skill" in ut:
        return "write_skill", {"name": "greet",
                              "description": "greeting procedure",
                              "content": "Say hello warmly and mention the weather."}
    if "elf-improvement review" in ut:  # case-tolerant tail match
        return "reflect", {"lesson": "improve loop works"}
    if "use-bad-args" in ut:
        return "run_shell", "__RAW__{\"command\":"
    if "use-landlock-escape" in ut:
        return "run_shell", {"command": f"echo pwned > {CANARY}"}
    if "use-landlock-write" in ut:
        return "run_shell", {"command": f"echo landlock-ok > {WORKSPACE}/landlock-canary2"}
    if "say the word mango" in ut:
        return "run_shell", {"command": "echo mango-delivered"}
    return "run_shell", {"command": "echo hello-from-tool"}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, obj, status=200):
        data = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass  # client exited while a test server was being shut down

    def do_GET(self):
        tg = re.match(r"^/bot([^/]+)/getMe$", self.path)
        if tg:
            self.tg_getme(tg.group(1))
            return
        if self.path.startswith("/_matrix/client/v3/account/whoami"):
            self._send({"user_id": "@bot:localhost"})
            return
        if self.path.startswith("/_matrix/client/v3/sync"):
            self.mx_sync(self.path)
            return
        mfile = re.match(r"^/file/bot([^/]+)/(.+)$", self.path)
        if mfile:
            body = b"\xff\xd8\xffMOCKJPEGDATA" * 8
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        mcdn = re.match(r"^/cdn/(.+)$", self.path)
        if mcdn:
            body = ("mock-file:" + mcdn.group(1)).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path == "/page":
            body = b"hello web page"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n)
        save_request(body)
        try:
            req = json.loads(body)
        except Exception:
            self._send({"error": {"message": "bad json"}}, 400)
            return

        tg = re.match(r"^/bot([^/]+)/(getMe|getUpdates|sendMessage|sendChatAction|editMessageText|answerCallbackQuery|getFile)$",
                      self.path)
        if self.path == "/v1/chat/completions":
            self.openai(req)
        elif self.path == "/v1/messages":
            self.anthropic(req)
        elif tg:
            token, method = tg.group(1), tg.group(2)
            if method == "getUpdates":
                self.tg_get_updates(req, token)
            elif method == "getMe":
                self.tg_getme(token)
            elif method == "getFile":
                self.tg_getfile()
            elif method == "sendMessage":
                self._send({"ok": True, "result": {"message_id": 1}})
            else:  # sendChatAction / editMessageText
                self._send({"ok": True, "result": True})
        elif self.path.startswith("/channels/") and self.path.endswith("/messages"):
            MSG_CTR[0] += 1
            self._send({"id": str(419430400 + 4194304 * MSG_CTR[0]), "content": "ok"})
        elif self.path.startswith("/interactions/") and self.path.endswith("/callback"):
            self._send({"id": "x"})
        else:
            # strict paths on purpose: double slashes or wrong routes must fail loudly
            self._send({"error": {"message": "unknown path " + self.path}}, 404)

    # ---- OpenAI-compatible ----
    def mx_event(self, room, body, sender="@alice:localhost"):
        return {"type": "m.room.message", "sender": sender, "event_id": "$e",
                "content": {"msgtype": "m.text", "body": body}}

    def mx_sync(self, path):
        auth = self.headers.get("Authorization", "")
        tok = auth.replace("Bearer ", "")
        since = ""
        if "since=" in path:
            since = path.split("since=")[1].split("&")[0]
        if tok in ("MXAUTH", "MXOTHER", "MXYES", "MXSTALE"):
            room = "!t:localhost"
            if since in ("", "0"):
                if tok == "MXAUTH":
                    ev = [self.mx_event(room, "attacker matrix", "@evil:localhost"),
                          self.mx_event(room, "missing matrix", None),
                          self.mx_event(room, "hello matrix")]
                else:
                    ev = [self.mx_event(room, f"run the command rm -rf {RISKY} now")]
                self._send({"next_batch": "s1", "rooms": {"join": {room: {"timeline": {"events": ev}}}}})
            elif since == "s1" and tok != "MXAUTH":
                def reply(sender, body, ref=True):
                    e = self.mx_event(room, body, sender)
                    if ref:
                        e["content"]["m.relates_to"] = {"m.in_reply_to": {"event_id": "$sent"}}
                    return e
                if tok == "MXOTHER":
                    ev = [reply("@bob:localhost", "/yes"), reply("@alice:localhost", "/no")]
                elif tok == "MXSTALE":
                    ev = [reply("@alice:localhost", "/yes", False), reply("@alice:localhost", "/no")]
                else:
                    ev = [reply("@alice:localhost", "/yes")]
                self._send({"next_batch": "s2", "rooms": {"join": {room: {"timeline": {"events": ev}}}}})
            else:
                self._send({"next_batch": "s3", "rooms": {"join": {}}})
        elif tok == "MXTEST":
            if since in ("", "0"):
                ev = [self.mx_event("!t:localhost", "hello matrix")]
                self._send({"next_batch": "s1", "rooms": {"join": {
                    "!t:localhost": {"timeline": {"events": ev}}}}})
            else:
                self._send({"next_batch": "s2", "rooms": {"join": {}}})
        elif tok == "MXROOMS":
            if since in ("", "0"):
                self._send({"next_batch": "s1", "rooms": {"join": {
                    "!nope:localhost": {"timeline": {"events":
                        [self.mx_event("!nope:localhost", "ignore me completely")]}},
                    "!ok:localhost": {"timeline": {"events":
                        [self.mx_event("!ok:localhost", "hello allowed")]}}}}})
            else:
                self._send({"next_batch": "s2", "rooms": {"join": {}}})
        else:
            self._send({"errcode": "M_UNKNOWN_TOKEN"}, 401)

    def tg_getfile(self):
        self._send({"ok": True, "result": {"file_id": "ph-big",
                    "file_path": "file/mock-photo.jpg", "file_size": 99}})

    def do_PUT(self):
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n)
        save_request(body)
        if self.path.startswith("/_matrix/client/v3/rooms/") and "/send/m.room.message" in self.path:
            self._send({"event_id": "$sent"})
        else:
            self._send({"error": {"message": "unknown path " + self.path}}, 404)

    def openai(self, req):
        tools = req.get("tools")
        if not tools:
            # tool-less probe (e.g. `clawc doctor`): plain text completion
            self._send({"choices": [{"message": {"role": "assistant", "content": "pong"}}]})
            return
        if "run_shell" not in json.dumps(tools):
            self._send({"error": {"message": "request missing tools"}}, 400)
            return
        msgs = req.get("messages", [])
        results = tool_results_openai(msgs)
        if results:
            reply = "FINAL: " + results[0].strip()
            if req.get("stream") and "stream-test" in first_user_text(msgs):
                self._send_openai_streamed(reply)
                return
            self._send({"choices": [{"message": {"role": "assistant",
                        "content": reply}}]})
            return
        ut = first_user_text(msgs)
        if "use-many-tools" in ut and "Learning pass" not in ut:
            calls = [{"id": "call_%d" % (k + 1), "type": "function",
                      "function": {"name": "run_shell",
                                   "arguments": json.dumps({"command": "echo burst-%d" % (k + 1)})}}
                     for k in range(5)]
            self._send({"choices": [{"message": {"role": "assistant", "content": None,
                        "tool_calls": calls}}]})
            return
        if "HEARTBEAT" in ut and f"rm -rf {RISKY}" not in ut:
            self._send({"choices": [{"message": {"role": "assistant", "content": "HEARTBEAT_OK"}}]})
            return
        name, args = llm_tool_for(ut)
        self._send({"choices": [{"message": {
            "role": "assistant", "content": None,
            "tool_calls": [{"id": "call_1", "type": "function",
                            "function": {"name": name,
                            "arguments": args[7:] if isinstance(args, str) and args.startswith("__RAW__")
                                         else json.dumps(args)}}]}}]})

    # ---- Anthropic ----
    def anthropic(self, req):
        if "run_shell" not in json.dumps(req.get("tools", [])) and \
           "load_skill" not in json.dumps(req.get("tools", [])):
            self._send({"error": {"message": "request missing tools"}}, 400)
            return
        msgs = req.get("messages", [])
        results = tool_results_anthropic(msgs)
        if results:
            self._send({"content": [{"type": "text",
                                     "text": "FINAL: " + results[0].strip()}],
                        "stop_reason": "end_turn"})
            return
        ut = first_user_text(msgs)
        if "skill-test" in ut:
            blk = {"type": "tool_use", "id": "toolu_1", "name": "load_skill",
                   "input": {"name": "example"}}
        else:
            blk = {"type": "tool_use", "id": "toolu_1", "name": "run_shell",
                   "input": {"command": "echo hello-from-tool"}}
        self._send({"content": [blk], "stop_reason": "tool_use"})

    def _send_openai_streamed(self, reply):
        """Split the reply into several SSE chunks so the client's on_delta
        callback fires many times in quick succession, exercising Telegram's
        edit throttle (the fast-stream regression path)."""
        n = len(reply)
        chunk = max(1, n // 4)
        parts = [reply[i:i + chunk] for i in range(0, n, chunk)]
        lines = []
        for p in parts:
            lines.append("data: " + json.dumps({"choices": [{"delta": {"content": p}}]}))
            lines.append("")
        lines.append("data: [DONE]")
        lines.append("")
        body = ("\n".join(lines)).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    # ---- Telegram ----
    def tg_get_updates(self, req, token):
        off = req.get("offset", 0)

        def msg(uid, text, chat_id=42):
            self._send({"ok": True, "result": [{
                "update_id": uid,
                "message": {"message_id": uid, "from": {"id": chat_id}, "chat": {"id": chat_id, "type": "private"},
                            "text": text},
            }]})

        if token == "TGAUTH":
            if off == 0:
                self._send({"ok": True, "result": [
                    {"update_id": 100, "message": {"message_id": 100, "from": {"id": 31337},
                     "chat": {"id": 777, "type": "private"}, "text": "/goal add attacker goal",
                     "photo": [{"file_id": "unauthorized-image"}]}},
                    {"update_id": 101, "message": {"message_id": 101,
                     "chat": {"id": 777, "type": "private"}, "text": "missing sender attack"}},
                    {"update_id": 102, "message": {"message_id": 102, "from": {"id": 777},
                     "chat": {"id": 1234, "type": "private"}, "caption": "wrong chat",
                     "photo": [{"file_id": "wrong-chat-image"}]}},
                    {"update_id": 103, "message": {"message_id": 103, "from": {"id": 777},
                     "chat": {"id": 777, "type": "private"}, "text": "/help"}}]})
            else: self.tg_empty()
        elif token in ("TGOTHERBTN", "TGOTHERTEXT", "TGSTALETEXT"):
            if off == 0:
                msg(100, f"run the command rm -rf {RISKY} now", 777)
            elif off == 101:
                if token == "TGOTHERBTN":
                    self.callback(101, 777, "conf:yes", sender=778)
                elif token == "TGOTHERTEXT":
                    self._send({"ok": True, "result": [{"update_id": 101, "message": {
                        "message_id": 101, "from": {"id": 778}, "chat": {"id": 777, "type": "private"}, "text": "/yes"}}]})
                else:
                    self._send({"ok": True, "result": [{"update_id": 101, "message": {
                        "message_id": 1, "from": {"id": 777}, "chat": {"id": 777, "type": "private"}, "text": "/yes"}}]})
            elif off == 102: self.callback(102, 777, "conf:no")
            else: self.tg_empty()
        elif token == "CONFIRMTOKEN":
            if off == 0:
                msg(100, f"run the command rm -rf {RISKY} now", 777)
            elif off == 101:
                self.callback(101, 777, "conf:yes")
            else:
                self.tg_empty()
        elif token == "CONFIRMNOTOKEN":
            if off == 0:
                msg(100, f"run the command rm -rf {RISKY} now", 888)
            elif off == 101:
                self.callback(101, 888, "conf:no")
            else:
                self.tg_empty()
        elif token == "CONFIRMSTALETOKEN":
            if off == 0:
                msg(100, f"run the command rm -rf {RISKY} now", 666)
            elif off == 101:
                self._send({"ok": True, "result": [{
                    "update_id": 101,
                    "callback_query": {
                        "id": "cbq-stale", "data": "conf:yes",
                        "from": {"id": 666},
                        "message": {"message_id": 999,
                                    "chat": {"id": 666, "type": "private"}},
                    }}]})
            else:
                self.tg_empty()
        elif token == "PHOTOTOKEN":
            if off == 0:
                self._send({"ok": True, "result": [{
                    "update_id": 100,
                    "message": {
                        "message_id": 501, "from": {"id": 300},
                        "chat": {"id": 300, "type": "private"},
                        "caption": "describe this picture",
                        "photo": [{"file_id": "ph-small", "file_size": 10},
                                  {"file_id": "ph-big", "file_size": 99}],
                    }}]})
            else:
                self.tg_empty()
        elif token == "GROUPTOKEN":
            if off == 0:
                self._send({"ok": True, "result": [{
                    "update_id": 100,
                    "message": {
                        "message_id": 601, "from": {"id": 400},
                        "chat": {"id": -100, "type": "group"},
                        "text": "ignore me completely",
                    }}]})
            elif off == 101:
                self._send({"ok": True, "result": [{
                    "update_id": 101,
                    "message": {
                        "message_id": 602, "from": {"id": 400},
                        "chat": {"id": -100, "type": "group"},
                        "text": "@mock_bot hello group",
                    }}]})
            else:
                self.tg_empty()
        elif token == "GOALCMDTOKEN":
            if off == 0:
                msg(100, "/goal add learn the drums", 701)
            elif off == 101:
                msg(200, "/goal", 702)
            elif off == 201:
                msg(300, "/goal done g1", 703)
            elif off == 301:
                msg(400, "/goal rm g1", 704)
            else:
                self.tg_empty()
        elif token == "CONFIRMNOMIDTOKEN":
            if off == 0:
                msg(100, f"run the command rm -rf {RISKY} now", 444)
            elif off == 101:
                self._send({"ok": True, "result": [{
                    "update_id": 101,
                    "callback_query": {
                        "id": "cbq-nomid", "data": "conf:yes",
                        "from": {"id": 444},
                        "message": {"chat": {"id": 444, "type": "private"}},
                    }}]})
            else:
                self.tg_empty()
        elif token == "CONFIRMTEXTTOKEN":
            if off == 0:
                msg(100, f"run the command rm -rf {RISKY} now", 999)
            elif off == 101:
                msg(101, "/yes", 999)
            else:
                self.tg_empty()
        elif token == "STATUSTOKEN":
            if off == 0:
                msg(100, "/status", 778)
            else:
                self.tg_empty()
        elif token == "STREAMTOKEN":
            if off == 0:
                msg(100, "stream-test now", 42)
            else:
                self.tg_empty()
        else:  # TESTTOKEN and default
            if off == 0:
                msg(100, "hello telegram", 42)
            elif off == 101 and token == "TESTTOKEN":
                msg(200, "hello again telegram", 42)
            else:
                self.tg_empty()

    def tg_getme(self, token):
        if token == "BADTOKEN":
            self._send({"ok": False, "error_code": 401,
                        "description": "Unauthorized"}, 401)
        else:
            self._send({"ok": True, "result": {"id": 1, "is_bot": True,
                        "first_name": "MockBot", "username": "mock_bot"}})

    def tg_empty(self):
        """Empty getUpdates result, after a short wait mimicking long polling
        (prevents test clients from tight-looping the mock)."""
        import time
        time.sleep(0.5)
        self._send({"ok": True, "result": []})

    def callback(self, uid, chat_id, data, sender=None):
        self._send({"ok": True, "result": [{
            "update_id": uid,
            "callback_query": {
                "id": "cbq-%d" % uid,
                "data": data,
                "from": {"id": sender if sender is not None else chat_id},
                "message": {"message_id": 1, "chat": {"id": chat_id, "type": "private"}},
            },
        }]})


MSG_CTR = [0]

class WsGateway(threading.Thread):
    """Scripted Discord gateway over WebSocket on port+1 (DCTEST token)."""
    daemon = True

    def __init__(self, port):
        super().__init__()
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", port))
        self.listener.listen(8)
        self.listener.settimeout(0.2)
        self.port = self.listener.getsockname()[1]
        self.stopped = threading.Event()
        self.clients = []
        self.events_sent = threading.Event()

    def close(self):
        self.stopped.set()
        self.join(timeout=3)
        self.listener.close()
        for client in self.clients:
            client.join(timeout=3)

    @staticmethod
    def frame(payload, opcode=1):
        b = payload if isinstance(payload, bytes) else payload.encode()
        hdr = bytes([0x80 | opcode])
        n = len(b)
        if n < 126:
            hdr += bytes([n])
        elif n < 65536:
            hdr += bytes([126, n >> 8, n & 255])
        else:
            hdr += bytes([127]) + n.to_bytes(8, "big")
        return hdr + b

    @staticmethod
    def read_frame(f):
        h = f.read(2)
        if not h or len(h) < 2:
            return None, None
        op = h[0] & 0x0F
        masked = (h[1] & 0x80) != 0
        n = h[1] & 0x7F
        if n == 126:
            n = int.from_bytes(f.read(2), "big")
        elif n == 127:
            n = int.from_bytes(f.read(8), "big")
        mask = f.read(4) if masked else b""
        data = f.read(n) if n else b""
        if masked:
            data = bytes(d ^ mask[i % 4] for i, d in enumerate(data))
        return op, data

    def handle(self, conn):
        f = conn.makefile("rb")
        req = b""
        while b"\r\n\r\n" not in req:
            chunk = f.readline()
            if not chunk:
                conn.close()
                return
            req += chunk
        key = ""
        for line in req.split(b"\r\n"):
            if line.lower().startswith(b"sec-websocket-key:"):
                key = line.split(b":", 1)[1].strip().decode()
        accept = base64.b64encode(hashlib.sha1(
            (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
        conn.sendall(("HTTP/1.1 101 Switching Protocols\r\n"
                      "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: %s\r\n\r\n" % accept).encode())
        conn.sendall(self.frame('{"op":10,"d":{"heartbeat_interval":10000}}'))
        while True:
            try:
                op, data = self.read_frame(f)
            except Exception:
                break
            if op is None or op == 8:
                break
            if op == 9:
                conn.sendall(self.frame(data, opcode=10))
                continue
            if op != 1:
                continue
            try:
                msg = json.loads(data)
            except Exception:
                continue
            m_op = msg.get("op")
            if m_op == 2:  # identify
                tok = (msg.get("d") or {}).get("token")
                if tok not in ("DCTEST", "DCBOT", "DCFLOW", "DCSTALE", "DCSLASH",
                               "DCMEDIA", "DCMEDIA2", "DCAUTH", "DCOTHERBTN", "DCOTHERTEXT", "DCSTALETEXT", "DCTEXTYES", "DCGUILD"):
                    break
                MSG_CTR[0] = 0  # deterministic message ids per scripted flow
                conn.sendall(self.frame(json.dumps(
                    {"op": 0, "s": 1, "t": "READY",
                     "d": {"session_id": "sess1", "application": {"id": "app1"}}})))
                if tok == "DCAUTH":
                    base = "http://127.0.0.1:%d/cdn/" % (self.port - 100)
                    events = [
                        ("MESSAGE_CREATE", {"id": "401", "channel_id": "111", "guild_id": "g1",
                         "content": "attacker discord", "author": {"id": "evil"},
                         "attachments": [{"url": base + "evil.png", "filename": "evil.png"}]}),
                        ("MESSAGE_CREATE", {"id": "402", "channel_id": "111", "guild_id": "g1",
                         "content": "missing discord", "author": {}}),
                        ("INTERACTION_CREATE", {"id": "ibad1", "token": "bad1", "type": 2,
                         "channel_id": "111", "user": {"id": "evil"},
                         "data": {"name": "claw", "options": [{"name": "task", "value": "attacker slash"}]}}),
                        ("INTERACTION_CREATE", {"id": "ibad2", "token": "bad2", "type": 2,
                         "channel_id": "111", "user": {"id": "evil"},
                         "data": {"name": "goal", "options": [{"name": "task", "value": "attacker goal"}]}}),
                        ("INTERACTION_CREATE", {"id": "ibad3", "token": "bad3", "type": 2,
                         "channel_id": "111", "data": {"name": "goal", "options": [{"name": "task", "value": "missing goal"}]}}),
                        ("INTERACTION_CREATE", {"id": "ispoof", "token": "tspoof", "type": 2,
                         "channel_id": "111", "guild_id": "g1", "user": {"id": "u1"},
                         "data": {"name": "goal", "options": [{"name": "task", "value": "spoofed guild goal"}]}}),
                        ("MESSAGE_CREATE", {"id": "403", "channel_id": "111", "guild_id": "g1",
                         "content": "/help", "author": {"id": "u1"}})]
                    for idx, (kind, data) in enumerate(events, 2):
                        conn.sendall(self.frame(json.dumps({"op": 0, "s": idx, "t": kind, "d": data})))
                if tok in ("DCOTHERBTN", "DCOTHERTEXT", "DCSTALETEXT", "DCTEXTYES"):
                    conn.sendall(self.frame(json.dumps({"op": 0, "s": 2, "t": "MESSAGE_CREATE", "d": {
                        "id": "419430400", "channel_id": "111", "guild_id": "g1",
                        "content": f"run the command rm -rf {RISKY} now", "author": {"id": "u1"}}})))
                    import time as _t
                    _t.sleep(2)
                    if tok == "DCOTHERBTN":
                        ev = {"id": "ibother", "token": "tother", "type": 3, "channel_id": "111",
                              "guild_id": "g1", "member": {"user": {"id": "u2"}},
                              "data": {"custom_id": "conf:yes"}, "message": {"id": "423624704"}}
                        conn.sendall(self.frame(json.dumps({"op": 0, "s": 3, "t": "INTERACTION_CREATE", "d": ev})))
                    else:
                        sender = "u2" if tok == "DCOTHERTEXT" else "u1"
                        mid = "419430400" if tok == "DCSTALETEXT" else "440401920"
                        ev = {"id": mid, "channel_id": "111", "guild_id": "g1",
                              "content": "/yes", "author": {"id": sender}}
                        conn.sendall(self.frame(json.dumps({"op": 0, "s": 3, "t": "MESSAGE_CREATE", "d": ev})))
                    if tok != "DCTEXTYES":
                        ev = {"id": "ibno", "token": "tno", "type": 3, "channel_id": "111",
                              "user": {"id": "u1"}, "data": {"custom_id": "conf:no"},
                              "message": {"id": "423624704"}}
                        conn.sendall(self.frame(json.dumps({"op": 0, "s": 4, "t": "INTERACTION_CREATE", "d": ev})))
                if tok in ("DCMEDIA", "DCMEDIA2"):
                    # HTTP and WebSocket listeners may use independently allocated ports.
                    base = "http://127.0.0.1:%d/cdn/" % SELF_PORT[0]
                    if tok == "DCMEDIA":
                        d = {"channel_id": "111",
                             "content": "what is this picture please",
                             "guild_id": "g1",
                             "attachments": [
                                 {"url": base + "test-pic.png",
                                  "filename": "test-pic.png",
                                  "content_type": "image/png", "size": 21},
                                 {"url": base + "note.txt",
                                  "filename": "note.txt",
                                  "content_type": "text/plain", "size": 16}],
                             "author": {"id": "u1", "bot": False}}
                    else:
                        d = {"channel_id": "111", "content": "",
                             "guild_id": "g1",
                             "attachments": [
                                 {"url": base + "solo.bin",
                                  "filename": "solo.bin",
                                  "content_type": "application/octet-stream",
                                  "size": 16}],
                             "author": {"id": "u1", "bot": False}}
                    conn.sendall(self.frame(json.dumps(
                        {"op": 0, "s": 2, "t": "MESSAGE_CREATE", "d": d})))
                if tok in ("DCTEST", "DCBOT", "DCFLOW", "DCSTALE"):
                    content = (f"run the command rm -rf {RISKY} now"
                               if tok not in ("DCTEST", "DCBOT") else "hello discord")
                    conn.sendall(self.frame(json.dumps(
                        {"op": 0, "s": 2, "t": "MESSAGE_CREATE",
                         "d": {"channel_id": "111", "content": content,
                               "guild_id": "g1",
                               "author": {"id": "u1", "bot": tok == "DCBOT"}}})))
                    self.events_sent.set()
                if tok == "DCFLOW":
                    import time as _t
                    _t.sleep(3)
                    conn.sendall(self.frame(json.dumps(
                        {"op": 0, "s": 3, "t": "INTERACTION_CREATE",
                         "d": {"id": "i1", "token": "itok1", "type": 3,
                               "data": {"custom_id": "conf:yes"},
                               "message": {"id": "423624704"},
                               "channel_id": "111", "user": {"id": "u1"}}})))
                elif tok == "DCSTALE":
                    import time as _t
                    _t.sleep(2)
                    conn.sendall(self.frame(json.dumps(
                        {"op": 0, "s": 3, "t": "INTERACTION_CREATE",
                         "d": {"id": "i2", "token": "itok2", "type": 3,
                               "data": {"custom_id": "conf:yes"},
                               "message": {"id": "999999999"},
                               "channel_id": "111", "user": {"id": "u1"}}})))
                elif tok == "DCGUILD":
                    conn.sendall(self.frame(json.dumps(
                        {"op": 0, "s": 2, "t": "INTERACTION_CREATE",
                         "d": {"id": "iguild", "token": "tguild", "type": 2,
                               "guild_id": "g1", "channel_id": "111",
                               "member": {"user": {"id": "u1"}},
                               "data": {"name": "claw", "options":
                                        [{"name": "task", "value": "say the word mango"}]}}})))
                elif tok == "DCSLASH":
                    conn.sendall(self.frame(json.dumps(
                        {"op": 0, "s": 2, "t": "INTERACTION_CREATE",
                         "d": {"id": "i3", "token": "itok3", "type": 2,
                               "data": {"name": "claw", "options":
                                        [{"name": "task", "value": "say the word mango"}]},
                               "channel_id": "111", "user": {"id": "u1"}}})))
                    self.events_sent.set()
            elif m_op == 1:  # heartbeat
                conn.sendall(self.frame('{"op":11,"d":null}'))
        conn.close()

    def run(self):
        while not self.stopped.is_set():
            try:
                conn, _ = self.listener.accept()
            except socket.timeout:
                continue
            client = threading.Thread(target=self.handle, args=(conn,), daemon=True)
            self.clients.append(client)
            client.start()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8873
    tls_port = int(sys.argv[2]) if len(sys.argv) > 2 else 8875
    SELF_PORT[0] = port
    open("/tmp/clawc-test-port", "w").write(str(port))

    WsGateway(port + 100).start()  # +1 would collide with serve tests
    http_srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    https_srv = ThreadingHTTPServer(("127.0.0.1", tls_port), Handler)

    cert = os.environ.get("CLAWC_TEST_CERT", "/tmp/clawc-test-tls/cert.pem")
    key = os.environ.get("CLAWC_TEST_KEY", "/tmp/clawc-test-tls/key.pem")
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    https_srv.socket = ctx.wrap_socket(https_srv.socket, server_side=True)

    import threading
    threading.Thread(target=http_srv.serve_forever, daemon=True).start()
    https_srv.serve_forever()
