# Nordstjernen Privacy Policy

This is the canonical privacy policy for the Nordstjernen Web
Navigator. Publish it verbatim at `https://nordstjernen.org/privacy`
— Microsoft Store policy 10.5.1 requires every Win32 product to link
a privacy policy from its listing (see `docs/windows-store.md`), and
other store fronts ask for the same URL.

---

**Effective date:** June 11, 2026

## Summary

Nordstjernen collects nothing. The browser has no telemetry, no
crash reporting, no update pinger, no analytics, no accounts, and no
"studies" infrastructure. It never phones home — the Nordstjernen
project operates no server that the browser talks to.

## Data the browser stores on your device

Like any web browser, Nordstjernen keeps your browsing data locally
so the browser works as you expect: history, bookmarks, cookies and
site storage, the page cache, per-site permission decisions (for
example camera access), and your settings. This data stays in your
user profile directory on your device, is never transmitted to the
Nordstjernen project or anyone else, and you can delete it at any
time from the browser's settings or by deleting the profile
directory.

## Network traffic the browser generates

Nordstjernen connects only to the sites you visit: the address you
type, the links you click, and the resources those pages reference.
Text typed into the search box is sent to your configured search
engine (DuckDuckGo Lite by default, changeable in settings).
Clicking a media overlay hands the media URL to your system's
external player. There are no background connections, prefetch
services, safe-browsing lookups, or other third-party services
built in.

Websites you visit receive the information any browser sends them
(your IP address, requested URLs, cookies they set) and are governed
by their own privacy policies.

## Privacy preference signals

To help you exercise your rights under laws such as the California
Consumer Privacy Act (CCPA/CPRA) and similar US state statutes,
Nordstjernen sends a Global Privacy Control signal by default — the
`Sec-GPC: 1` request header and the `navigator.globalPrivacyControl`
property — which many jurisdictions treat as a legally binding request
to opt out of the sale or sharing of your personal information. It also
sends the legacy Do Not Track signal (`DNT: 1` and
`navigator.doNotTrack`) by default. Both signals are independently
toggleable in Settings. Honouring these signals is the responsibility
of the websites that receive them.

By default Nordstjernen also blocks third-party cookies, strips common
tracking parameters from URLs, upgrades connections to HTTPS, and coarsens
script-visible high-resolution timers (such as `performance.now()`) to
limit timing-based fingerprinting and side-channel attacks.

## Children

Nordstjernen provides unfiltered access to the web and is not
directed at children.

## Changes

Changes to this policy are published at the URL above and recorded
in the project's public source repository, which also serves as the
verifiable history of this document.

## Contact

Questions about this policy: open an issue on the Nordstjernen
source repository, or use the contact details published at
`https://nordstjernen.org`.
