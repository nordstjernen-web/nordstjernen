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
  release notes. The auto-updater fetches its manifest from here.
- **Earn trust.** A source-available browser needs to look
  professional. Plain HTML, no tracking, no cookies, no
  JavaScript. The website itself should render fine in
  Nordstjernen — dogfooding from day one.

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
/license                  The FSL-1.1-MIT license text + plain-English summary.
/about                    Same prose as about:nordstjernen, plus contact + GPG key.
/changelog                Release notes, manually curated from NORDSTJERNEN.md log.
/manifest.json            Auto-updater manifest (small JSON: latest version + URLs).
```

The first cut is the landing + download + manifest.
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
- **No checkout, no accounts, no server-side state.** The
  browser is free under FSL-1.1-MIT; there is nothing to sell on
  the site and nothing to log in to. Commercial inquiries
  (competing-use licensing, paid support, custom integrations)
  go to a plain `mailto:` link on `/about`.

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
3. **What is the catch?** None for personal, internal, academic,
   or non-competing commercial use — source-available under
   FSL-1.1-MIT, MIT after ten years per release. Only shipping
   Nordstjernen inside a competing browser product needs a
   separate license.
4. **Get it.** One big button: *Download*.

Below the fold: the design constraints (one human's worth of
code, no plugins, no WebGL, no telemetry, English only) so
people self-select before downloading.

## Things the website lets the browser do

Once the site exists, several deferred items in `NORDSTJERNEN.md`
land cleanly:

- **Phase 11 auto-updater.** Browser fetches
  `https://nordstjernen.org/manifest.json` once per 24h, compares
  versions, shows a non-blocking popup if newer-by-30-days.
- **`about:nordstjernen` link target.** Already linked from
  the about page; just point at the live site.

## Things the website should never do

- Read or write cookies.
- Run third-party JavaScript (ads, analytics, support widgets,
  embedded chat). The whole site is static HTML.
- Have a privacy policy longer than two paragraphs.
- Show a cookie banner. There are no cookies to disclose.
- A/B test anything.

## Open questions

- **Commercial-licensing inquiries.** FSL-1.1-MIT covers
  everything except shipping Nordstjernen inside a competing
  browser product. If a company wants that, route them to a
  contact email and negotiate per-deal. No public pricing.
- **Trademark policy.** "Nordstjernen" is a Norwegian word for
  the North Star — generic enough that we will not police it.
  The logo (when there is one) is the trademark surface.
