# nordstjernen.org — website plan

This document is the plan for the project's public website at
`https://nordstjernen.org`. The domain is registered as of
2026-05-12. The browser identifies as
`Nordstjernen/0.0.1 (+https://nordstjernen.org)` and the
`about:nordstjernen` page links here, so the site is on the
critical path for the project's first impression — but it can
start as a single static page and grow from there.

## Goals

- **Be the canonical project URL.** Stable, owner-controlled,
  not dependent on a particular forge. The GitHub repo can move,
  the website doesn't have to. The User-Agent and About page
  both point here.
- **Distribute the binaries.** Downloads page, signed
  Windows installer + macOS DMG + Linux tarball, checksums,
  release notes. Phase 11 (shareware distribution) needs a
  hosting location for the manifest the auto-updater fetches —
  this is it.
- **Sell the licenses.** Stripe Checkout / Lemon Squeezy /
  similar — single one-time purchase, key issued by email,
  no account required. Suppresses the in-app nag. The page that
  takes payment is the *only* server-side surface the project
  needs to operate the shareware model.
- **Earn trust.** Source-available browser asking for money
  needs to look professional. Plain HTML, no tracking, no
  cookies on the marketing pages, no JavaScript on anything
  other than the checkout flow. The website itself should
  render fine in Nordstjernen — dogfooding from day one.

## Non-goals

- Forum / comment system / accounts / login. Email is enough.
- Telemetry / analytics. Server logs only if needed for abuse
  triage; nothing client-side.
- A blog with frequent posts. The iteration log in
  `NORDSTJERNEN.md` is enough; the website covers releases.
- Translations. English only, matching the browser.
- Newsletter signups, popups, cookie banners, anything that
  pushes against the same minimalism the browser stands for.

## Initial sitemap

```
/                         Landing page: what Nordstjernen is, screenshot, download CTA.
/download                 Per-OS download buttons + checksums + signing info.
/license                  Buy a license: pricing, Stripe Checkout link, what unlocks.
/license/verify           Server endpoint that issues / re-issues keys against email.
/about                    Same prose as about:nordstjernen, plus contact + GPG key.
/changelog               Release notes, manually curated from NORDSTJERNEN.md log.
/manifest.json            Auto-updater manifest (small JSON: latest version + URLs).
```

The first cut is the landing + download + license + manifest.
Everything else can wait until a real user asks for it.

## Tech choices

- **Static site, no framework.** Hand-written HTML + a single
  CSS file. Build is `rsync site/ host:`. If a generator
  appears later, prefer one that emits clean HTML and exits
  (Hugo / Zola).
- **Hosting.** Any static host with HTTPS + custom domain.
  Cloudflare Pages, Netlify, GitHub Pages with a CNAME — all
  fine. Cost is zero; bandwidth budget is generous.
- **TLS.** Cert from the host's free Let's Encrypt integration.
  HSTS preload — the browser already enforces HSTS for hosts
  it has seen, but the *public* HSTS preload list owned by
  Chromium et al. is the standard belt-and-braces.
- **Checkout.** Stripe Checkout in hosted mode (no PCI scope on
  our side). Or Lemon Squeezy if Stripe isn't available in the
  developer's jurisdiction. Either issues a webhook on success;
  the webhook generates a signed license key and emails it via
  Postmark / Resend / Amazon SES.
- **License key generation.** Ed25519 sign the email + purchase
  timestamp + purchase ID. Public key baked into the browser
  binary; the browser verifies the key locally and never phones
  home. Format: `nordstjernen-base64(payload).base64(sig)`.
- **No accounts.** The email + license key is the only thing
  the buyer needs. Lost the key? The verify endpoint will
  re-send it given the original payment email.

## Content for the landing page

The landing page should answer four questions, in this order, in
a couple of paragraphs each:

1. **What is this?** A small web browser written in C from
   scratch. One screenshot of a real page. The list of "real
   things it does" (HTML5, modern CSS, pragmatic JS, no
   telemetry, no plugins, etc.).
2. **Why would I want it?** Reading the text-heavy web —
   Wikipedia, news, search, docs — without the bloat. Privacy
   by construction. Audit-the-source.
3. **What is the catch?** Shareware. Free download, polite nag
   after a while. License removes the nag. Reference to Opera's
   early model.
4. **Get it.** Two big buttons: *Download* and *Buy a license*.

Below the fold: the design constraints (one human's worth of
code, no plugins, no WebGL, no telemetry, English only) so
people self-select before downloading.

## Things the website lets the browser do

Once the site exists, several deferred items in `NORDSTJERNEN.md`
land cleanly:

- **Phase 11 auto-updater.** Browser fetches
  `https://nordstjernen.org/manifest.json` once per 24h, compares
  versions, shows a non-blocking popup if newer-by-30-days.
- **Phase 11 license-key flow.** Buyer's flow ends with the
  signed key in their inbox; About dialog inside the browser
  takes the key, calls `nd_license_verify`, and from then on
  the nag is suppressed.
- **`about:nordstjernen` link target.** Already linked from
  the about page; just point at the live site.

## Things the website should never do

- Read or write cookies on marketing pages.
- Run third-party JavaScript (ads, analytics, support widgets,
  embedded chat). The checkout flow can run Stripe's own JS
  because that is what `/license` is *for*; everywhere else is
  static HTML.
- Have a privacy policy longer than two paragraphs.
- Show a cookie banner. There are no cookies to disclose.
- A/B test anything.

## Open questions

- **Pricing.** $19 one-time? $29? Track what early Opera asked
  for in real dollars and pick something defensible. Lower is
  better for trust; higher is better for being able to support
  development.
- **Refund policy.** 30-day no-questions-asked, stated plainly.
- **VAT / sales tax.** Stripe Tax / Lemon Squeezy as
  merchant-of-record removes most of this burden; otherwise the
  developer collects + remits locally.
- **Trademark policy.** "Nordstjernen" is a Norwegian word for
  the North Star — generic enough that we will not police it.
  The logo (when there is one) is the trademark surface.
