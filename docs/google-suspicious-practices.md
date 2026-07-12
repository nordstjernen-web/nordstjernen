# Google Search and the browsers it locks out

*An opinion piece from the Nordstjernen project, 2026-07-12.*

Nordstjernen is a web browser written from scratch. That makes it a
rare thing on today's web: a client Google has never seen before. This
document records, with reproducible observations, how Google Search
treats such a client — and argues that the effect, whatever the
intent, is anti-competitive.

## What we observe

All of the following was captured on 2026-07-12 with Nordstjernen's
headless renderer, from a single host. The Google **homepage** renders
and works: the engine runs Google's own JavaScript, dismisses the
consent dialog, and produces a pixel-faithful page
(`docs/screenshot.png`).

The **search results page** is another story:

1. Any request to `https://www.google.com/search?q=…` — the very URL
   the homepage's own search box submits to — is answered not with
   results but with an "unusual traffic" interstitial
   (`google.com/sorry`): *"Our systems have detected unusual traffic
   from your computer network."* The page embeds a reCAPTCHA
   checkbox.

2. Nordstjernen renders that checkbox correctly and executes the
   reCAPTCHA widget's JavaScript in its iframe. Clicking "I'm not a
   robot" runs the verification animation, the widget accepts the
   solve, and the page redirects back to the search URL (with a fresh
   `sei=` token).

3. That redirect is answered by **the same interstitial again**, with
   a new CAPTCHA. Solving it repeats the cycle. There is no exit: the
   proof of humanity is accepted and then ignored. We verified this
   loop repeatedly, including against the "basic HTML" variant
   (`gbv=1`) of the results page.

So Google's stack, on the same connection and within the same minute,
serves its homepage to Nordstjernen without complaint, but flatly
refuses to serve a single search result — while dangling a
verification mechanism that does not actually unlock anything.

## Why this matters for competition

An anti-bot system that (a) keys its suspicion on how *familiar* the
client looks, and (b) offers a recovery path that doesn't work, is
functionally indistinguishable from a policy of refusing service to
unfamiliar browsers. The distinction between "we block abusive
traffic" and "we block traffic that doesn't look like Chrome" is
precisely the distinction between security engineering and gatekeeping
— and the broken CAPTCHA loop collapses it. A user who has just proved
they are human and is still turned away has not been risk-scored; they
have been turned away.

The incentives line up badly. Google operates the dominant search
engine *and* the dominant browser. Every heuristic that treats "not
Chrome-shaped" as "suspicious" — an unrecognized user-agent string, an
unfamiliar TLS or JavaScript fingerprint, a missing years-old cookie
trail — raises the cost of using a competing or new browser, and
raises it most for the smallest engines, which is exactly where
browser-engine diversity would have to come from. A new engine cannot
acquire a trustworthy "reputation" without users, and cannot serve
users who are met with an error page on the web's most-used site.

This is not a new pattern. The web spent a decade digging out of
user-agent sniffing the last time around; the industry consensus that
came out of it — feature detection, not client discrimination — is
written into every modern web standard. Reputation-based gating moves
the same discrimination down the stack, where a small engine cannot
even see what it is being judged on, let alone fix it.

## The usual caveats, honestly

Bot protection is real and necessary; search results are scraped at
enormous scale, and the network this test ran from is a hosting
network, which no doubt lowers the trust score before the browser
sends its first byte. We do not claim to know Google's intent, and we
do not claim the gate was built to exclude competitors.

But effect matters more than intent. The homepage works; results do
not. The CAPTCHA solves; the wall stays. Mainstream browsers on
mainstream connections glide through the same risk engine every day
without ever seeing it. When the cost of false positives lands this
asymmetrically on non-mainstream clients — and when the appeal
mechanism is a treadmill — the system functions as a moat around the
default browser, whatever it was built for.

## Reproducing

```sh
./builddir/src/gtk/nordstjernen --headless \
    --dump=png:serp.png --settle-ms=5000 \
    --act="click 47,104; wait 20000" \
    "https://www.google.com/search?q=nordstjernen+web+browser"
```

The log shows the reCAPTCHA solve and the follow navigation; the dump
shows the interstitial served again. The homepage, by contrast,
renders end-to-end: see `docs/screenshot.png`.

## What we ask

Nothing exotic: that verification, once passed, unlocks the service;
that risk engines be audited for disparate impact on non-dominant
clients; and that "works in any conforming browser" remain a property
of the open web rather than a courtesy extended to the two or three
engines big enough to be on an allowlist.
