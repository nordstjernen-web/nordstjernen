# compatibility-css

Per-site CSS overrides applied on top of the page's own stylesheets. The
browser looks up a stylesheet for the current host and appends it to the
cascade after the page's inline `<style>` and external `<link>` sheets,
so rules here win on the same specificity.

## Layout

One file per site. Filenames are referenced by the rules table in
`src/compatibility.c`:

| File              | Hosts                                  |
| ----------------- | -------------------------------------- |
| `google.css`      | `google.<tld>` and subdomains          |
| `duckduckgo.css`  | `duckduckgo.com` and subdomains        |
| `wikipedia.css`   | `wikipedia.org` and subdomains         |
| `aftenposten.css` | `aftenposten.no` and subdomains        |
| `reddit.css`      | `reddit.com` and subdomains            |

## Search path

When the browser needs a file it tries, in order:

1. `$XDG_DATA_HOME/nordstjernen/compatibility-css/<file>` (user override)
2. `./compatibility-css/<file>` (run from project root)
3. `./data/compatibility-css/<file>`
4. `../share/nordstjernen/compatibility-css/<file>` (installed beside the
   executable)
5. `/usr/local/share/nordstjernen/compatibility-css/<file>`
6. `/usr/share/nordstjernen/compatibility-css/<file>`

A user can drop a file with the matching name into the user data
directory to override the bundled rules without rebuilding.

## Adding a new site

1. Create `<site>.css` here.
2. Add a `compat_rule` entry in `src/compatibility.c` with a host matcher
   and the filename.
3. Optionally set a `user_agent` override on the same rule, or attach a
   DOM rewriter for tricks that CSS can't express.
