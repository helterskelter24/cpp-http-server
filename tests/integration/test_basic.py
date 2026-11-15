"""Basic request/response behaviour of the built-in routes and static files."""

import json
import os

from harness import WWW, check, get, read_response, req, shared


@shared
def test_get_health(srv):
    r = srv.request(get("/health"))
    check(r.status == 200, r)
    check(r.body == b'{"status":"ok"}', r.body)
    check(r.headers.get("content-type", "").startswith("application/json"), r.headers)
    check(r.headers.get("server") == "cpp-http-server", r.headers)


@shared
def test_static_index(srv):
    r = srv.request(get("/"))
    check(r.status == 200, r)
    check(r.headers.get("content-type", "").startswith("text/html"), r.headers)
    with open(os.path.join(WWW, "index.html"), "rb") as f:
        check(r.body == f.read(), "index body differs from www/index.html")


@shared
def test_static_16k(srv):
    r = srv.request(get("/file16k.bin"))
    check(r.status == 200, r)
    check(len(r.body) == 16384, len(r.body))
    with open(os.path.join(WWW, "file16k.bin"), "rb") as f:
        check(r.body == f.read(), "file16k.bin body differs from the file")


@shared
def test_head_static(srv):
    with srv.connect() as s:
        s.sendall(req("HEAD", "/file16k.bin"))
        r = read_response(s, head=True)
        check(r.status == 200, r)
        check(r.headers.get("content-length") == "16384", r.headers)
        # If the server had sent a body anyway, those bytes would be parsed as
        # the start of this second response and fail the status-line check.
        s.sendall(get("/health"))
        r2 = read_response(s)
        check(r2.status == 200 and r2.body == b'{"status":"ok"}', r2)


@shared
def test_post_echo(srv):
    r = srv.request(req("POST", "/api/echo", body=b"hello"))
    check(r.status == 200, r)
    doc = json.loads(r.body)
    check(doc["method"] == "POST" and doc["body"] == "hello" and doc["body_length"] == 5, doc)


@shared
def test_large_body_at_limit(srv):
    # Exactly the default --max-body-bytes (1 MiB): accepted whole, even
    # though it arrives over many reads.
    body = bytes(ord("a") + i % 26 for i in range(1048576))
    r = srv.request(req("POST", "/api/echo", body=body))
    check(r.status == 200, r)
    doc = json.loads(r.body)
    check(doc["body_length"] == len(body), doc["body_length"])
    check(doc["body"].encode() == body, "echoed body differs")


@shared
def test_users_param(srv):
    r = srv.request(get("/api/users/42"))
    check(r.status == 200, r)
    doc = json.loads(r.body)
    check(doc["id"] == "42" and doc["name"] == "user-42", doc)


@shared
def test_stats_counts(srv):
    for _ in range(3):
        check(srv.request(get("/health")).status == 200, "health failed")
    r = srv.request(get("/api/stats"))
    check(r.status == 200, r)
    doc = json.loads(r.body)
    check(doc["requests_total"] >= 3, doc)
    check(doc["mode"] == srv.mode, doc)
    check(doc["connections_accepted"] >= 4, doc)
    # The connection carrying this very request is open while the stats are read.
    check(doc["active_connections"] >= 1, doc)
