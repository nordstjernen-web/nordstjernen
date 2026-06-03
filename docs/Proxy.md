# Proxies and VPNs in Nordstjernen

Nordstjernen routes all HTTP/HTTPS traffic through libcurl, so any
proxy libcurl understands works: plain HTTP proxies, HTTPS-to-the-proxy
("HTTPS proxy"), SOCKS4, SOCKS5, and SOCKS5 with remote DNS
(`socks5h://`, recommended whenever you don't want DNS leaks).

There is no separate VPN client built into the browser. A real VPN
(WireGuard, OpenVPN, Tailscale, your employer's IPSec client, the
system VPN pane on macOS and Windows) is an OS-level routing
concern — once it's up, every connection nordstjernen opens already
goes through it, with no browser configuration needed. The proxy
support below is the browser-side complement: useful when you want
to send only browser traffic through a tunnel, point at Tor, or talk
to a corporate forward proxy.

## Configuring a proxy

Three ways, in priority order (highest wins):

1. **Command line** (one-shot, doesn't touch the config file):

   ```sh
   nordstjernen --proxy=socks5h://127.0.0.1:1080
   nordstjernen --proxy=http://user:pass@proxy.corp:8080
   ```

2. **Environment variables** (per-shell):

   ```sh
   export ND_HTTPS_PROXY=socks5h://127.0.0.1:1080
   export ND_HTTP_PROXY=http://proxy.corp:8080
   export ND_NO_PROXY=localhost,127.0.0.1,*.corp.internal
   ```

   `ND_HTTPS_PROXY` is used for `https://` URLs, `ND_HTTP_PROXY` for
   plain `http://`. If only `ND_HTTP_PROXY` is set, it is used for
   both schemes.

3. **Config file** at `~/.config/nordstjernen/nordstjernen.conf`
   (Linux and macOS — the path comes from GLib's user-config dir,
   which is `$XDG_CONFIG_HOME` or `~/.config`), or under the local
   AppData directory on Windows
   (`%LOCALAPPDATA%\nordstjernen\nordstjernen.conf`):

   ```ini
   http_proxy  = http://proxy.corp:8080
   https_proxy = socks5h://127.0.0.1:1080
   no_proxy    = localhost,127.0.0.1,*.corp.internal
   ```

If none of the three are set, libcurl falls back to its own
auto-detection of the standard `http_proxy` / `HTTPS_PROXY` /
`NO_PROXY` lowercase env vars, so an existing shell-wide proxy
setup keeps working.

Run `nordstjernen --print-config` to confirm the effective values;
passwords in the printed proxy URLs are masked.

## Supported proxy URL schemes

- `http://[user:pass@]host:port` — classic HTTP proxy. `CONNECT` is
  used for HTTPS targets, so the proxy sees only the host name, not
  the path or body.
- `https://[user:pass@]host:port` — TLS tunnel to the proxy itself.
- `socks4://host:port`, `socks4a://host:port` — SOCKS4, SOCKS4 with
  remote DNS.
- `socks5://host:port` — SOCKS5 with local DNS. **Leaks DNS** to
  your normal resolver; usually not what you want.
- `socks5h://host:port` — SOCKS5 with remote DNS. The proxy resolves
  the hostname. Use this for Tor (`socks5h://127.0.0.1:9050` is the
  standard Tor SOCKS port) and for any SSH dynamic forward.

## Recipes

### Route browser traffic through an SSH tunnel

This is the "poor person's VPN" — anything you can SSH to becomes a
SOCKS5 endpoint:

```sh
ssh -D 1080 -N user@jump.example.com &
nordstjernen --proxy=socks5h://127.0.0.1:1080
```

DNS is resolved on the far side of the SSH connection. Kill the SSH
process to "disconnect."

### Route browser traffic through Tor

```sh
nordstjernen --proxy=socks5h://127.0.0.1:9050
```

assuming `tor` is running locally with default settings. This is not
a substitute for Tor Browser — Nordstjernen does not implement the
anti-fingerprinting hardening Tor Browser does — but it's enough for
reading text-mode sites behind Tor.

### Corporate forward proxy with credentials

```sh
export ND_HTTP_PROXY=http://alice:hunter2@proxy.corp:8080
export ND_HTTPS_PROXY=http://alice:hunter2@proxy.corp:8080
export ND_NO_PROXY=localhost,127.0.0.1,*.corp.internal
nordstjernen
```

Or, equivalently, put the same keys in `nordstjernen.conf` and
launch with no flags.

## Limitations

- WebSocket connections (`ws://`, `wss://`) use the same libcurl
  config and are tunneled through the same proxy.
- PAC (proxy auto-config) scripts are not interpreted. Resolve to a
  concrete proxy URL manually.
- The `no_proxy` list is matched by libcurl exactly the way curl's
  `--noproxy` does: comma-separated host suffixes, with `*` as a
  bypass-everything wildcard.
