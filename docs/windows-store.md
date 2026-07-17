# Nordstjernen in the Microsoft Store

available in Microsoft store here now:
https://apps.microsoft.com/detail/9nw8t7w5z4pl


This document records the research, the packaging work, and the
submission procedure for shipping Nordstjernen through the Microsoft
Store on Windows 10/11. It complements `docs/Windows.md` (build,
portable bundle, NSIS installer); everything below builds on that
existing Windows packaging.

Policy references are to the
[Microsoft Store Policies](https://learn.microsoft.com/en-us/windows/apps/publish/store-policies),
version 7.19, effective October 14, 2025 — re-check the live page
before every submission, the version number bumps several times a
year.




Nordstjernen is an independent engine — neither Chromium nor Gecko —
so a straightforward "Web browser" submission **fails certification
on 10.2.1 as written**. The policy was introduced in June 2022 (it
replaced the even stricter pre-Windows-11 rule that browsers had to
use the OS-provided engine) and is unchanged through policy version
7.19. There is no documented exception process.

What this means in practice:

1. **Do not change the engine.** Adopting Chromium or Gecko is
   antithetical to the project (`CLAUDE.md`: no upstream engine
   code, ever). The engine is the product.
2. **Request an exception before submitting.** The policy's stated
   intent is security currency, which Nordstjernen meets on its own
   terms (open source, in-tree engine, fast release cadence). Raise
   the question through Partner Center → Help + support with a
   pre-submission inquiry referencing 10.2.1, and via
   `reportapp@microsoft.com`. Independent-engine browsers (Ladybird,
   Servo-based shells) face the same wall; regulatory pressure that
   already forced engine-choice openness elsewhere (e.g. iOS in the
   EU under the DMA) may move this policy over time.
3. **Submit and let certification decide.** Certification feedback
   is concrete and citable; a rejection note that names 10.2.1 is
   the artifact to attach to an exception request or escalation.
4. **Everything else should be ready regardless.** All of the
   packaging and compliance work below is policy-10.2.1-independent
   and is also what an eventual `winget`/MSIX sideload distribution
   needs, so none of it is wasted.

Until an exception lands, the Store-independent channels remain the
NSIS installer, the portable ZIP (`docs/Windows.md`), and a `winget`
manifest — the `winget-pkgs` community repo has no browser-engine
policy and accepts the existing NSIS installer as-is. Once a
versioned installer is hosted at a stable URL, submission is one
command (it generates the manifest trio and opens the PR):

```powershell
wingetcreate new https://nordstjernen.org/dl/nordstjernen-1.0.17-win64-setup.exe
```

## Packaging: MSIX

The Store accepts two technology families for desktop apps: MSIX
packages, or unmodified MSI/EXE installers. MSIX is the right choice
for Nordstjernen:

- The Store **signs MSIX packages itself** after certification — no
  Authenticode certificate purchase, which the MSI/EXE path
  requires.
- Clean per-user install/uninstall managed by Windows (policy 10.2.7
  satisfied by construction), automatic updates, no ARP/registry
  writes of our own.
- Per-user by design, which matches the browser's policy of never
  running with Administrator rights — it drops them if it is launched
  elevated (`src/security.c::ns_security_refuse_root`).

[Package requirements](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/app-package-requirements)
that shaped the manifest:

- Version must be 4-part numeric `Major.Minor.Build.Revision` with
  **Revision = 0** (the Store reserves the fourth part). Meson's
  `1.0.19` maps to `1.0.19.0`.
- `Package/Identity/Name` and `Publisher` must exactly match the
  values Partner Center assigns for the reserved Store product
  **Nordstjernen Web Browser**. The script defaults match the
  current Partner Center identity.
- Max package size 25 GB (ours is ~117 MB staged, ~49 MB packed;
  the `NS_MSIX_AI=enabled` variant that bundles the statically-linked
  llama.cpp runtime is ~88 MB packed).
- Full-trust desktop apps declare
  `TargetDeviceFamily Name="Windows.Desktop"` with
  `MinVersion 10.0.17763.0` (1809, the MSIX baseline — same floor
  Firefox's Store package uses) and the `runFullTrust` restricted
  capability.

### What's in the tree

- `data/msix/AppxManifest.xml.in` — manifest template. Declares:
  - `runFullTrust` (the bundle is a normal Win32 process tree: the
    launcher `nordstjernen.exe` execs `app/nordstjernen-ui.exe`,
    which spawns one sandboxed `app/nordstjernen-renderer.exe` per
    tab — child processes inherit the package identity, no changes
    needed).
  - `windows.protocol` extensions for **http** and **https**, which
    is what makes a packaged app appear in Windows Settings →
    Default apps as a selectable browser. Protocol activation passes
    the URI as a command-line argument, which the binary already
    accepts. (Same mechanism Firefox's MSIX uses.)
  - `windows.fileTypeAssociation` for `.htm .html .xht .xhtml .svg`.
  - `windows.appExecutionAlias` so `nordstjernen.exe` works from a
    console regardless of install path.
  - A single `en-US` package resource. UI translation is in-app
    (`src/i18n.c` picks the OS language at startup), not
    Windows-resource-based, so declaring 40 package languages would
    only multiply the Partner Center listing burden without changing
    runtime behavior.
- `scripts/pack-msix.sh` — runs `scripts/pack-windows.sh`, stages
  the bundle plus tile PNGs rendered from
  `data/icons/hicolor/scalable/apps/nordstjernen.svg` with
  `rsvg-convert`: StoreLogo (50), Square44x44, Square71x71,
  Square150x150 and Square310x310, each in `scale-100/125/150/200/400`
  variants, plus `targetsize-16/24/32/48/256` (and
  `altform-unplated`) variants of Square44x44 for taskbar and
  Start-list rendering. If `makepri.exe` is available the variants
  are indexed into `resources.pri` (without it Windows only uses
  the base-named assets — functional, but Partner Center warns
  about missing scale-200). Substitutes the manifest placeholders
  and packs with `makeappx.exe` from the Windows 10/11 SDK
  (auto-located under
  `C:\Program Files (x86)\Windows Kits\10\bin\*\x64\`) or the
  cross-platform `makemsix` CLI as fallback. Output:
  `dist/nordstjernen-<version>-win64.msix`, unsigned. `makeappx`
  validates the manifest against the package schema, so a
  successful pack doubles as a manifest check.
- CI: the Windows workflow (`.github/workflows/windows.yml`) runs
  `pack-msix.sh` on every push to `main` (GitHub's Windows runners
  carry the Windows SDK) and uploads the unsigned `.msix` as an
  artifact, so every merge revalidates the manifest and produces an
  installable-after-signing package.

Partner Center product identity:

```sh
NS_MSIX_IDENTITY_NAME='29567TheFreecivProject.NordstjernenWebBrowser' \
NS_MSIX_PUBLISHER='CN=631F98F7-2280-49EE-8EF8-534CC36D09CF' \
NS_MSIX_PUBLISHER_DISPLAY='Nordstjernen' \
NS_MSIX_PHONE_PRODUCT_ID='2c47a178-dfb0-4383-9dc0-aa7195bc8354' \
NS_MSIX_PHONE_PUBLISHER_ID='eb62046e-1fa9-48a1-b651-cbf7237e9a03' \
./scripts/pack-msix.sh
```

These are the script defaults. `NS_MSIX_DISPLAY_NAME` defaults to
`Nordstjernen Web Browser`, matching the reserved Store listing name.
Override any of them only for a different Partner Center product.
`NS_MSIX_IDENTITY_NAME` must remain the exact Partner Center-assigned
package identity, even when its prefix does not match the current
customer-facing publisher name. Store validation rejects packages with
renamed identities because the Package Family Name is derived from this
value and the publisher certificate subject.
The `mp:PhoneIdentity` values are included for Store submission;
Microsoft's manifest schema requires both phone GUID fields, while
the desktop package identity still comes from `Package/Identity`.

Store discovery values:

- Store ID: `9NW8T7W5Z4PL`
- Store URL: `https://apps.microsoft.com/detail/9NW8T7W5Z4PL`
- Store protocol link:
  `ms-windows-store://pdp/?productid=9NW8T7W5Z4PL`
- Package Family Name:
  `29567TheFreecivProject.NordstjernenWebBrowser_ga6t65cntcpba`

Restricted capability approval text:

```text
Capability requested: runFullTrust

Nordstjernen Web Browser is a packaged Win32 desktop browser. The
capability is required only to launch the bundled desktop executable and
its renderer subprocesses from the MSIX package. The app does not request
elevation, install services or drivers, modify system settings, manage
other packages, mine cryptocurrency, or collect telemetry.
```

### Local testing (sideload)

The Store package is submitted unsigned, but Windows refuses to
install an unsigned MSIX locally. Two options:

1. **Loose-layout registration** (fastest, needs Developer Mode in
   Windows Settings, no signing):

   ```powershell
   Add-AppxPackage -Register dist\nordstjernen-msix\AppxManifest.xml
   ```

2. **Self-signed package**: create a test cert whose Subject equals
   the manifest `Publisher`, sign, trust, install:

   ```powershell
   New-SelfSignedCertificate -Type Custom -CertStoreLocation Cert:\CurrentUser\My `
     -Subject 'CN=631F98F7-2280-49EE-8EF8-534CC36D09CF' `
     -KeyUsage DigitalSignature -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3')
   # export to .pfx, then:
   #   NS_MSIX_CERT_PFX=/c/path/test.pfx ./scripts/pack-msix.sh
   Add-AppxPackage dist\nordstjernen-1.0.17-win64.msix
   ```

Verify after install: app launches from Start, appears under
Settings → Default apps → Web browser, `nordstjernen.exe` resolves
in a fresh terminal, pages load over HTTPS (the CA bundle rides
inside the package), and uninstall from Start removes it cleanly.

### MSIX runtime differences to be aware of

- The package installs under `C:\Program Files\WindowsApps\<id>\`
  (system-managed, read-only). The browser already treats its
  install dir as read-only and bootstraps `GTK_DATA_PREFIX`,
  pixbuf-loader and CA-bundle paths relative to the exe
  (`ns_win32_anchor_gtk_data`), so nothing changes.
- `%LOCALAPPDATA%` writes are **virtualized** to
  `%LOCALAPPDATA%\Packages\<identity>\LocalCache\Local\…`. Config,
  profile and the `nordstjernen-debug.log` all keep working, they
  just live under the package container path — relevant when asking
  users for logs.
- Windows associates http/https through the manifest declarations
  above; the app must **not** write `HKCU` registry associations
  itself (it doesn't).
- MSIX uninstall removes the package container. No NSIS, no ARP
  entries — the NSIS installer remains the non-Store channel and the
  two installs coexist without conflict (different install roots,
  different ARP sources).

## Store compliance checklist (policy 7.19)

| Policy | Requirement | Nordstjernen status |
|---|---|---|
| 10.1 | Accurate title/description/screenshots, distinct value, unique title | Listing copy to be written from `README.md`; screenshots via `scripts/render-screenshots.sh` |
| 10.2.1 | Browsers must use Chromium or Gecko | **Blocker — see above; exception required** |
| 10.2.2/3 | No malware, no undisclosed software, dependencies disclosed | Self-contained bundle, no downloads, no secondary installs |
| 10.2.6 | No cryptomining | None |
| 10.2.7 | Clean uninstall | MSIX-managed |
| 10.5.1 | Privacy policy — *mandatory for all Win32/full-trust products*, regardless of data collection | Canonical text ready in `docs/privacy-policy.md` — **action item:** publish it at `nordstjernen.org/privacy` and paste that URL into the submission |
| 10.5.x | No data sold/shared | True by construction — nothing collected |
| 10.8 | Commerce | Free app, no in-app purchases — nothing to do |
| 10.13 | Gaming/Xbox | N/A |
| 11.x | Content policies | Age rating via the IARC questionnaire during submission; browsers with unfiltered web access rate 16+/18+ in most rating systems — answer the questionnaire honestly, the rating is what it is |

## Submission procedure

1. **Developer account** at
   [Partner Center](https://partner.microsoft.com/dashboard/registration):
   one-time fee, ~19 USD individual / ~99 USD company. Company
   accounts get publisher verification, which avoids the
   "unverified publisher" treatment.
2. **Reserve the app name**: **Nordstjernen Web Browser**.
   Reservation is what mints the real `Identity/Name` and
   `Publisher` GUID.
3. **Build the package** with the Partner Center identity defaults
   above; upload the `.msix` in a new
   submission. Do **not** sign it — the Store does that. The MSIX
   packaging path builds with `-Dai=disabled` by default, so the
   Store package has no model downloader, AI chat surface, or
   llama.cpp runtime. Set `NS_MSIX_AI=enabled` to build a package
   that bundles the local AI chat / llama.cpp runtime (statically
   linked, CPU-only unless `ai_gpu` is also enabled).
4. **Properties**: category *Productivity* (no dedicated browser
   category), system requirements: Windows 10 1809+, x64.
5. **Age rating**: complete the IARC questionnaire (unfiltered
   internet access → expect 16+).
6. **Privacy**: paste the published privacy policy URL — the
   submission form rejects Win32 apps without one. The text to
   publish is `docs/privacy-policy.md`.
7. **Listing** (en-US at minimum): description from `README.md`
   product vision, 3–5 screenshots (`scripts/render-screenshots.sh`
   output is the right source), the `nordstjernen.svg` logo renders
   for any requested promotional sizes.
8. **Submission notes for the certification tester**: state that
   the app is a web browser with its own open-source engine,
   reference any pre-submission 10.2.1 correspondence, and note the
   first-run behavior (opens the search-only `about:start`, no
   account, no setup).
9. Certification takes ~24–72 h. If rejected on 10.2.1, attach the
   rejection to an exception request (see blocker section) rather
   than resubmitting unchanged.

## Appendix: listing copy (ready to paste)

**Display name:** Nordstjernen Web Browser

**Short description** (≤100 chars):

> A minimalist, secure web browser with its own engine. No
> telemetry, no bloat — just the web.

**Description:**

> Nordstjernen is a web browser written from scratch — its own
> HTML, CSS and JavaScript engine, not another Chromium shell. The
> result is a browser that is small, fast to start, and simple
> enough for one person to read and audit.
>
> - **Private by construction.** No telemetry, no crash reporting,
>   no update pinger, no accounts. Nordstjernen never phones home;
>   the only servers it talks to are the sites you visit.
> - **Secure by default.** Every tab renders in its own isolated
>   process. WebGL is off until you trust a site. If it is launched
>   with administrator rights, the browser drops them.
> - **Modern web, no bloat.** HTML5, modern CSS and JavaScript,
>   WebAssembly, WebCrypto and opt-in WebGL — without WebGPU, ad
>   tech, or AI surface area. Audio and video hand off to the
>   player you already have.
> - **Yours.** Open source, with your history, cookies and settings
>   stored only on your device. Search defaults to DuckDuckGo Lite
>   and is one setting away from anything else.
>
> The UI speaks your language: the interface translates to the
> operating-system language automatically, with 40 languages
> in the box.

**Product features** (Store bullets):

- Independent browser engine — not Chromium, not a fork
- Per-tab renderer process isolation
- Zero telemetry, zero phone-home connections
- Opt-in, per-site WebGL trust prompts
- In-box UI translation for 40 languages
- Small footprint, fast startup

**Search terms** (max 7):
`web browser`, `private browser`, `lightweight browser`,
`no telemetry`, `independent engine`, `open source`, `secure browser`

**Category:** Productivity. **Pricing:** Free.

## Sources

- [Microsoft Store Policies (7.19)](https://learn.microsoft.com/en-us/windows/apps/publish/store-policies) — 10.1, 10.2.1, 10.5.1, 10.8
- [MSIX app package requirements](https://learn.microsoft.com/en-us/windows/apps/publish/publish-your-app/msix/app-package-requirements)
- [Distribute a Win32 app through the Store](https://learn.microsoft.com/en-us/windows/apps/distribute-through-store/how-to-distribute-your-win32-app-through-microsoft-store)
- [Firefox MSIX package docs](https://firefox-source-docs.mozilla.org/browser/installer/windows/installer/MSIX.html) and its
  [`AppxManifest.xml.in`](https://searchfox.org/firefox-main/source/browser/installer/windows/msix/AppxManifest.xml.in) — the reference for browser protocol/file-type registration under MSIX
- [The Register on the 2022 browser-engine policy](https://www.theregister.com/2022/07/08/microsoft_store_open_source_webkit/)
