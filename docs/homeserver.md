# The homeserver: setting up the chat

Chat runs over [Matrix](https://matrix.org). The kiosk logs in as its own account (`@kiosk:…`) and has one room with each family member; the family uses any Matrix app (for example [Element](https://element.io)) on their phones. Any homeserver that speaks the client-server API works. This page sets up a small private [Synapse](https://element-hs.github.io/synapse/) with open registration off, behind a TLS reverse proxy.

```
kiosk / family phones
        │  https://matrix.example.org        (public name, valid certificate)
        ▼
reverse proxy (Caddy, nginx, …)  ── terminates TLS
        │  http://127.0.0.1:8008             (host loopback only)
        ▼
synapse container :8008                      (plain HTTP, x_forwarded: true)
```

Everything Synapse needs is in [homeserver/](../homeserver): a `docker-compose.yml`, a bootstrap script, a room script and an example Caddy config. Any machine with Docker and `docker-compose` can host it (a home server, a small VPS, an always-on desktop).

## Requirements

- Docker with the standalone `docker-compose` (the scripts call `docker-compose`; with only the `docker compose` plugin, alias one to the other).
- A DNS name for the homeserver, e.g. `matrix.example.org`, pointing at the machine, and a way for the kiosk and the phones to reach it over HTTPS (ports 80/443 open to them, a VPN, or a LAN-only name with a certificate). The kiosk verifies the certificate, so a self-signed one won't do unless you add its CA to the kiosk's trust store.
- `curl`, `openssl` and `python3` on the host (used by the scripts).

The **server name** is the part after `:` in every user ID (`@kiosk:matrix.example.org`). Synapse can't change it later, so pick it before the first start.

## Fresh install

> [!CAUTION]
> `setup-synapse.sh` **wipes `homeserver/files/`** (database, signing key, media). It refuses when `files/homeserver.db` exists unless `FORCE_WIPE=1`. Never run it against a server in use; change a running server by editing `files/homeserver.yaml` and recreating the container.

```bash
export SERVER_NAME=matrix.example.org
bash homeserver/setup-synapse.sh
```

It:

1. generates `files/homeserver.yaml` with the Synapse image's own generator (`generate`): SQLite database, a fresh signing key and secrets, a plain-HTTP listener on 8008 with `x_forwarded: true`, and **`enable_registration` off**, so nobody can sign up from outside; accounts are only made with the registration shared secret;
2. starts Synapse from [homeserver/docker-compose.yml](../homeserver/docker-compose.yml), published on `127.0.0.1:8008` only;
3. registers the `kiosk` account with `register_new_matrix_user` (inside the container), logs it in and prints its password and **access token**;
4. asks for family usernames and registers each, printing a generated password.

For a bigger install (PostgreSQL, federation, media limits), see the [Synapse docs](https://element-hs.github.io/synapse/latest/setup/installation.html); the kiosk only needs the client-server API.

### TLS: a reverse proxy

Synapse speaks plain HTTP on loopback; put a reverse proxy in front that terminates TLS. With [Caddy](https://caddyserver.com) the whole config is [homeserver/Caddyfile.example](../homeserver/Caddyfile.example); Caddy gets and renews a Let's Encrypt certificate by itself:

```
matrix.example.org {
	reverse_proxy /_matrix/* 127.0.0.1:8008
	reverse_proxy /_synapse/client/* 127.0.0.1:8008
}
```

With nginx, proxy `location ~ ^(/_matrix|/_synapse/client)` to `http://127.0.0.1:8008` with `proxy_set_header X-Forwarded-For $remote_addr;`, `proxy_set_header X-Forwarded-Proto $scheme;`, `proxy_set_header Host $host;` and `client_max_body_size 50M;` (photos), and get a certificate with certbot. Synapse's [reverse proxy guide](https://element-hs.github.io/synapse/latest/reverse_proxy.html) has full examples for both.

Check it from another machine (no `-k`: the certificate must verify):

```bash
curl -sS https://matrix.example.org/_matrix/client/versions
```

### Rooms between the family and the kiosk

Once the family members have signed in to their Matrix app once (homeserver `https://matrix.example.org`, the username and password printed above):

```bash
bash homeserver/setup-matrix-rooms.sh https://matrix.example.org kiosk '<kiosk_password>' parent1 parent2
```

It creates a private direct room between `kiosk` and each user (they get an invite to accept) and prints the `contact.N.*` lines for the `[contacts]` section of `/etc/chatbox.conf` ([configuration.md](configuration.md#contacts)). Then put `homeserver_url = https://matrix.example.org` and the kiosk's `access_token` in the same file.

Rooms can also be made by hand: from a family member's app, create a room and invite `@kiosk:matrix.example.org`; then make the kiosk join it with its token (`curl -X POST -H "Authorization: Bearer <token>" https://matrix.example.org/_matrix/client/v3/join/<room_id>`), since the kiosk doesn't accept invites itself. The room ID is in the room's settings (Advanced). Keep the rooms unencrypted: the kiosk doesn't do end-to-end encryption.

## Adding an account

Never use `setup-synapse.sh` for this: it wipes the server. Register the account on the running server with the tool shipped in the Synapse image. In `homeserver/`:

```bash
docker-compose exec synapse register_new_matrix_user -c /data/homeserver.yaml http://localhost:8008
```

It reads `registration_shared_secret` from `homeserver.yaml` and prompts for the username, the password and "Make admin [no]". `http://localhost:8008` is Synapse inside the container, so no TLS is involved. Non-interactive:

```bash
docker-compose exec synapse register_new_matrix_user -c /data/homeserver.yaml \
  -u <username> -p '<password>' --no-admin http://localhost:8008
```

`-p` leaves the password in the shell history; leave it out to be prompted. Neither the family accounts nor the kiosk need to be admins. If you want a Synapse admin for the [admin API](https://element-hs.github.io/synapse/latest/usage/administration/admin_api/), make a separate account with `-a`.

### A new family member

1. Register their account (above). They sign in to their Matrix app once.
2. Create their room with the kiosk, naming only the new user:
   ```bash
   bash homeserver/setup-matrix-rooms.sh https://matrix.example.org kiosk '<kiosk_password>' <username>
   ```
   It only creates rooms for the names given, so existing rooms are untouched. Naming an existing user again gives them a second room.
3. They accept the invite.
4. Add one contact to `/etc/chatbox.conf` on the kiosk with the room ID from the script's output (renumber `N`; up to 10 contacts), then `sudo systemctl restart chatbox`.

The kiosk's `access_token` doesn't change: the script logs in with its own session and logs out again.

### Getting an access token

An account that needs a token (the kiosk, or a replacement after a token was revoked) gets one by logging in:

```bash
curl -sS -X POST https://matrix.example.org/_matrix/client/v3/login \
  -H 'Content-Type: application/json' \
  -d '{"type":"m.login.password","identifier":{"type":"m.id.user","user":"kiosk"},"password":"<password>","initial_device_display_name":"chatbox"}'
```

The reply's `access_token` is the token. Every login is a new device with its own token; existing ones keep working until logged out. Treat it like a password: anyone holding it can read and send as the kiosk.

## Operating it

In `homeserver/`:

- **Recreate after a config change:** `docker-compose up -d --force-recreate`. The image is `:latest`; pin a version tag in `docker-compose.yml` if you'd rather upgrade on purpose.
- **Health:** `curl -fsS http://127.0.0.1:8008/health` on the host; `docker-compose logs synapse`.
- **Did a message reach the server?** Query the database read-only:
  `sqlite3 -readonly "file:files/homeserver.db?mode=ro" "SELECT datetime(origin_server_ts/1000,'unixepoch'), sender FROM events WHERE room_id='<room_id>' ORDER BY stream_ordering DESC LIMIT 5;"`
- **Backups:** everything is in `homeserver/files/` (database, signing key, media). Stop the container (or use `sqlite3 .backup`) and copy the folder.
- **Certificate:** `echo | openssl s_client -connect matrix.example.org:443 -servername matrix.example.org 2>/dev/null | openssl x509 -noout -issuer -dates` (`notAfter` should be well in the future; Caddy and certbot renew on their own).

## Troubleshooting

**The kiosk logs `SSL peer certificate or SSH remote key was not OK`.** A certificate problem on the server side, not a firmware bug: expired, self-signed, or for another name. Check with the `openssl` command above and a strict `curl` from the kiosk. Queued messages are sent on their own once it's fixed (the outbox retries forever).

**HTTP 401 on the kiosk.** The `access_token` is wrong or was revoked. Log in again as `kiosk` ([Getting an access token](#getting-an-access-token)), update `/etc/chatbox.conf` and `sudo systemctl restart chatbox`.

**Messages don't arrive.** Check, in order: the kiosk reaches the server (`curl -sS https://matrix.example.org/_matrix/client/versions` on the kiosk); the reverse proxy is up and forwards to `127.0.0.1:8008`; Synapse is up (`docker ps`, the health check); `docker-compose logs synapse`; the room IDs in `/etc/chatbox.conf` match the rooms and the family member has joined.

**"Empty reply from server" or 502 from the proxy.** Synapse speaks plain HTTP; the proxy's upstream must be `http://`, not `https://`.

**Photos don't arrive.** Raise the proxy's body size limit (nginx `client_max_body_size`) and Synapse's `max_upload_size` if phones send big photos; the kiosk itself accepts up to 25 MB ([photos.md](photos.md)).
