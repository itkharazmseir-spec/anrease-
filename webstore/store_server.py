#!/usr/bin/env python3
"""
AndreasOnline web store - a small, dependency-free (Python stdlib only)
local website that lets a player log in with their existing game account
(the same username/password their AndreasOnline.cfg uses to connect) and
"buy" (free, no real payment - see README.md next to this file) a weapon
or vehicle, which gets delivered to them in-game via the exact same
purchases.txt mechanism documented in docs/DEPLOYMENT.md / docs/MEMORY.md
of the main AndreasOnline repo.

WHERE TO RUN THIS: copy this whole `webstore/` folder next to
AndreasOnlineServer.exe (so the default relative paths "accounts.dat" and
"purchases.txt" point at the same files the game server already reads and
writes), then:

    python store_server.py

and open http://localhost:8080 in a browser (or http://<server LAN
IP>:8080 from another PC on the same network).

HOW LOGIN WORKS: this does NOT create accounts. An account only exists
once a player has connected to the game at least once with
account_user=/account_password= set in their AndreasOnline.cfg (see
server/src/AccountStore.h in the main repo - that's what writes
accounts.dat). This site only reads that same file to check the
password (hashed the same way, plain SHA-256, no salt - same disclosed
limitation as the game client/server already have) and look up the
player's accountId.

HOW A PURCHASE IS DELIVERED: buying an item appends one line to
purchases.txt in the exact format server/src/Main.cpp's PollPurchaseQueue
already expects (accountId|itemKind|itemId|amount) - the dedicated game
server picks it up within its existing 2-second poll and delivers it IF
that account is currently connected to the game. If not connected, the
dedicated server logs and drops it (a pre-existing, disclosed limitation
of the purchase-delivery feature itself, not something this site changes).

HONEST LIMITATIONS OF THIS PROTOTYPE (read before exposing this beyond
your own LAN):
- No real payment of any kind - every "price" shown is cosmetic/fake.
  Nothing here moves money. If real payments are ever wanted, that is a
  separate, much bigger piece of work (a real payment processor,
  currency handling, fraud/chargeback concerns) that this does NOT do.
- Login sessions are a random token kept in this process's memory only -
  they vanish if you restart store_server.py, and there is no password
  rate-limiting or account lockout.
- Plain HTTP, not HTTPS - fine on your own LAN, NOT fine to expose
  directly to the public internet as-is (passwords would travel
  unencrypted). Put it behind a reverse proxy with TLS first if you ever
  want it reachable from outside your network.
- Single-process, single-file storage (accounts.dat / purchases.txt) -
  same "small community server" scale this whole project already is, not
  built for many concurrent buyers.
"""

import argparse
import hashlib
import json
import os
import secrets
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
STATIC_DIR = os.path.join(SCRIPT_DIR, "static")

# itemKind values must match shared/include/ao/Protocol.h's PurchaseItemKind.
KIND_VEHICLE = 0
KIND_WEAPON = 1

# Catalog is defined here (server-side) on purpose - the browser only ever
# sends a catalog "id" when buying, never a raw itemKind/itemId/amount, so a
# tampered request can at worst buy a listed item, never an arbitrary one.
#
# Weapon ids are eWeaponType values already relied on elsewhere in this
# project (see client/src/LocalPlayer.cpp's WeaponSlotFor() table) - public,
# well-documented GTA SA values, not individually re-verified here.
# Vehicle model ids are well-known public GTA SA model ids; only 400
# (Landstalker) has actually been live-tested end to end so far (see
# docs/MEMORY.md, item 7) - the others are offered on the same trust basis
# as the weapon ids but haven't been individually confirmed spawning
# correctly on this build.
CATALOG = [
    {"id": "knife",      "name": "Knife",              "price": 50,   "kind": KIND_WEAPON,  "itemId": 3,  "amount": 1},
    {"id": "bat",         "name": "Baseball Bat",        "price": 75,   "kind": KIND_WEAPON,  "itemId": 5,  "amount": 1},
    {"id": "pistol",      "name": "Pistol",              "price": 200,  "kind": KIND_WEAPON,  "itemId": 22, "amount": 60},
    {"id": "shotgun",     "name": "Shotgun",              "price": 350,  "kind": KIND_WEAPON,  "itemId": 25, "amount": 40},
    {"id": "ak47",        "name": "AK-47",                "price": 650,  "kind": KIND_WEAPON,  "itemId": 30, "amount": 250},
    {"id": "m4",          "name": "M4",                   "price": 700,  "kind": KIND_WEAPON,  "itemId": 31, "amount": 250},
    {"id": "rpg",         "name": "Rocket Launcher",      "price": 1500, "kind": KIND_WEAPON,  "itemId": 35, "amount": 10},
    {"id": "landstalker", "name": "Landstalker (SUV)",    "price": 1200, "kind": KIND_VEHICLE, "itemId": 400, "amount": 0},
    {"id": "bravura",     "name": "Bravura (sedan)",      "price": 1000, "kind": KIND_VEHICLE, "itemId": 401, "amount": 0},
    {"id": "banshee",     "name": "Banshee (sports car)", "price": 2500, "kind": KIND_VEHICLE, "itemId": 429, "amount": 0},
    {"id": "infernus",    "name": "Infernus (supercar)",  "price": 4000, "kind": KIND_VEHICLE, "itemId": 411, "amount": 0},
]
CATALOG_BY_ID = {item["id"]: item for item in CATALOG}


def normalize_username(username: str) -> str:
    """Mirrors server/src/AccountStore.h's AccountStore::Normalize() exactly:
    trim whitespace, lowercase. Must stay in sync with that function."""
    return username.strip().lower()


def sha256_hex(password: str) -> str:
    """Mirrors shared/include/ao/Sha256.h: plain SHA-256 of the UTF-8
    password bytes, lowercase hex digest. No salt (same disclosed
    limitation the game client/server already carry)."""
    return hashlib.sha256(password.encode("utf-8")).hexdigest()


class AccountsFile:
    """Read-only view of accounts.dat (server/src/AccountStore.h's format:
    accountId|username|sha256HashHex|createdAtUnixSeconds). This site never
    writes to this file - account creation only ever happens from the game
    client/server (see server/src/Main.cpp)."""

    def __init__(self, path: str):
        self.path = path

    def find(self, username: str):
        wanted = normalize_username(username)
        if not os.path.exists(self.path):
            return None
        with open(self.path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.rstrip("\r\n")
                if not line:
                    continue
                parts = line.split("|")
                if len(parts) != 4:
                    continue
                account_id_str, username_field, hash_hex, _created = parts
                if normalize_username(username_field) != wanted:
                    continue
                try:
                    account_id = int(account_id_str)
                except ValueError:
                    continue
                return {"accountId": account_id, "username": normalize_username(username_field), "hashHex": hash_hex.lower()}
        return None


class PurchaseQueue:
    """Appends one line per purchase to purchases.txt, in the exact format
    server/src/Main.cpp's PollPurchaseQueue expects. A lock guards concurrent
    writes from multiple logged-in browsers hitting this process at once."""

    def __init__(self, path: str):
        self.path = path
        self._lock = threading.Lock()

    def enqueue(self, account_id: int, kind: int, item_id: int, amount: int):
        line = f"{account_id}|{kind}|{item_id}|{amount}\n"
        with self._lock:
            with open(self.path, "a", encoding="utf-8") as f:
                f.write(line)


class SessionStore:
    """In-memory only - tokens are lost on restart (see module docstring's
    disclosed limitations). Each token maps to the accountId/username that
    logged in; the server trusts this mapping, never a client-supplied
    accountId, when enqueuing a purchase."""

    SESSION_LIFETIME_SECONDS = 24 * 3600

    def __init__(self):
        self._sessions = {}
        self._lock = threading.Lock()

    def create(self, account_id: int, username: str) -> str:
        token = secrets.token_hex(32)
        with self._lock:
            self._sessions[token] = {
                "accountId": account_id,
                "username": username,
                "expiresAt": time.time() + self.SESSION_LIFETIME_SECONDS,
            }
        return token

    def resolve(self, token: str):
        if not token:
            return None
        with self._lock:
            session = self._sessions.get(token)
            if not session:
                return None
            if session["expiresAt"] < time.time():
                del self._sessions[token]
                return None
            return session


def make_handler(accounts: AccountsFile, purchases: PurchaseQueue, sessions: SessionStore):
    class Handler(BaseHTTPRequestHandler):
        server_version = "AndreasOnlineStore/1.0"

        def log_message(self, fmt, *args):
            print(f"[webstore] {self.address_string()} - {fmt % args}")

        def _send_json(self, status: int, payload: dict):
            body = json.dumps(payload).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _read_json_body(self):
            length = int(self.headers.get("Content-Length", "0") or "0")
            if length <= 0:
                return {}
            raw = self.rfile.read(length)
            try:
                return json.loads(raw.decode("utf-8"))
            except (json.JSONDecodeError, UnicodeDecodeError):
                return None

        def _bearer_token(self):
            auth = self.headers.get("Authorization", "")
            if auth.startswith("Bearer "):
                return auth[len("Bearer "):].strip()
            return None

        # ---- static file serving (index.html / style.css / app.js) ----
        def _serve_static(self, rel_path: str):
            if rel_path in ("", "/"):
                rel_path = "index.html"
            rel_path = rel_path.lstrip("/")
            full_path = os.path.normpath(os.path.join(STATIC_DIR, rel_path))
            if not full_path.startswith(os.path.normpath(STATIC_DIR)):
                self.send_error(403)
                return
            if not os.path.isfile(full_path):
                self.send_error(404)
                return
            content_type = "text/html"
            if full_path.endswith(".css"):
                content_type = "text/css"
            elif full_path.endswith(".js"):
                content_type = "application/javascript"
            with open(full_path, "rb") as f:
                body = f.read()
            self.send_response(200)
            self.send_header("Content-Type", content_type + "; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        # ---------------------------- routes ----------------------------
        def do_GET(self):
            path = urlparse(self.path).path
            if path == "/api/catalog":
                self._send_json(200, {"items": [
                    {"id": i["id"], "name": i["name"], "price": i["price"],
                     "kind": "vehicle" if i["kind"] == KIND_VEHICLE else "weapon"}
                    for i in CATALOG
                ]})
                return
            if path == "/api/me":
                session = sessions.resolve(self._bearer_token())
                if not session:
                    self._send_json(401, {"error": "not logged in"})
                    return
                self._send_json(200, {"accountId": session["accountId"], "username": session["username"]})
                return
            self._serve_static(path)

        def do_POST(self):
            path = urlparse(self.path).path
            body = self._read_json_body()
            if body is None:
                self._send_json(400, {"error": "malformed JSON body"})
                return

            if path == "/api/login":
                username = str(body.get("username", ""))
                password = str(body.get("password", ""))
                if not username or not password:
                    self._send_json(400, {"error": "username and password are required"})
                    return
                account = accounts.find(username)
                if not account or account["hashHex"] != sha256_hex(password):
                    # Same message either way - don't reveal whether the
                    # username exists.
                    self._send_json(401, {"error": "wrong username or password"})
                    return
                token = sessions.create(account["accountId"], account["username"])
                self._send_json(200, {"token": token, "accountId": account["accountId"], "username": account["username"]})
                return

            if path == "/api/purchase":
                session = sessions.resolve(self._bearer_token())
                if not session:
                    self._send_json(401, {"error": "not logged in"})
                    return
                catalog_id = str(body.get("catalogId", ""))
                item = CATALOG_BY_ID.get(catalog_id)
                if not item:
                    self._send_json(400, {"error": "unknown item"})
                    return
                purchases.enqueue(session["accountId"], item["kind"], item["itemId"], item["amount"])
                print(f"[webstore] purchase: accountId={session['accountId']} "
                      f"username={session['username']} item={item['id']}")
                self._send_json(200, {"ok": True, "message":
                    f"{item['name']} queued - it'll arrive in-game within a few seconds "
                    f"if you're currently connected and playing."})
                return

            self._send_json(404, {"error": "no such endpoint"})

    return Handler


def main():
    parser = argparse.ArgumentParser(description="AndreasOnline web store")
    parser.add_argument("--port", type=int, default=8080)
    # Default to the folder THIS SCRIPT's parent directory (i.e. one level up
    # from webstore/) rather than the current working directory - this file
    # is meant to live at <server folder>/webstore/store_server.py, so its
    # parent is where AndreasOnlineServer.exe actually writes accounts.dat /
    # purchases.txt. Using cwd instead was a real bug found during testing:
    # running `python store_server.py` from inside webstore/ made it look for
    # accounts.dat in webstore/ itself (where it doesn't exist), so every
    # login silently failed with "wrong username or password" even for a
    # correct, existing account. --accounts/--purchases can still override
    # this explicitly if you deploy this somewhere else.
    server_folder = os.path.dirname(SCRIPT_DIR)
    parser.add_argument("--accounts", default=os.path.join(server_folder, "accounts.dat"),
                         help="Path to the game server's accounts.dat (default: ../accounts.dat, i.e. next to AndreasOnlineServer.exe)")
    parser.add_argument("--purchases", default=os.path.join(server_folder, "purchases.txt"),
                         help="Path to the game server's purchases.txt (default: ../purchases.txt, i.e. next to AndreasOnlineServer.exe)")
    args = parser.parse_args()

    print(f"[webstore] accounts file: {args.accounts}")
    print(f"[webstore] purchases file: {args.purchases}")
    if not os.path.exists(args.accounts):
        print("[webstore] WARNING: accounts file does not exist yet - no one has "
              "logged into the game with account_user=/account_password= set yet, "
              "so no one can log in here either until they do.")

    accounts = AccountsFile(args.accounts)
    purchases = PurchaseQueue(args.purchases)
    sessions = SessionStore()
    handler_cls = make_handler(accounts, purchases, sessions)

    httpd = ThreadingHTTPServer(("0.0.0.0", args.port), handler_cls)
    print(f"[webstore] listening on http://0.0.0.0:{args.port} "
          f"(open http://localhost:{args.port} on this PC, or http://<this PC's LAN IP>:{args.port} from another)")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[webstore] shutting down")
        httpd.shutdown()


if __name__ == "__main__":
    main()
