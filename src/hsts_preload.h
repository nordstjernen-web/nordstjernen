/* Nordstjernen — small curated HSTS preload list (host + include-subdomains).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: FSL-1.1-MIT
 */

#ifndef ND_HSTS_PRELOAD_H
#define ND_HSTS_PRELOAD_H

typedef struct nd_hsts_preload_entry {
    const char *host;
    int         include_subdomains;
} nd_hsts_preload_entry;

static const nd_hsts_preload_entry nd_hsts_preload[] = {
    { "nordstjernen.org",       1 },

    { "google.com",             1 },
    { "googleapis.com",         1 },
    { "gstatic.com",            1 },
    { "youtube.com",            1 },
    { "youtu.be",               1 },
    { "ytimg.com",              1 },
    { "googleusercontent.com",  1 },
    { "googlemail.com",         1 },
    { "gmail.com",              1 },

    { "duckduckgo.com",         1 },
    { "duck.com",               1 },

    { "microsoft.com",          0 },
    { "live.com",               1 },
    { "outlook.com",            1 },
    { "office.com",             1 },
    { "office365.com",          1 },
    { "microsoftonline.com",    1 },
    { "sharepoint.com",         1 },
    { "onedrive.com",           1 },
    { "hotmail.com",            1 },
    { "bing.com",               0 },

    { "apple.com",              0 },
    { "icloud.com",             1 },

    { "amazon.com",             0 },
    { "aws.amazon.com",         1 },

    { "github.com",             1 },
    { "githubusercontent.com",  1 },
    { "github.io",              1 },
    { "gitlab.com",             1 },
    { "codeberg.org",           1 },
    { "sourcehut.org",          1 },
    { "sr.ht",                  1 },

    { "facebook.com",           1 },
    { "messenger.com",          1 },
    { "instagram.com",          1 },
    { "whatsapp.com",           1 },
    { "fb.com",                 1 },

    { "twitter.com",            1 },
    { "x.com",                  1 },
    { "t.co",                   1 },

    { "linkedin.com",           1 },
    { "reddit.com",             1 },
    { "redditmedia.com",        1 },
    { "pinterest.com",          1 },
    { "yahoo.com",              0 },

    { "wikipedia.org",          1 },
    { "wikimedia.org",          1 },
    { "wiktionary.org",         1 },
    { "wikidata.org",           1 },

    { "stackoverflow.com",      1 },
    { "stackexchange.com",      1 },
    { "stackauth.com",          1 },

    { "mozilla.org",            1 },
    { "mozilla.com",            1 },
    { "firefox.com",            1 },
    { "cloudflare.com",         1 },
    { "cloudflare-dns.com",     1 },
    { "letsencrypt.org",        1 },

    { "protonmail.com",         1 },
    { "proton.me",              1 },
    { "tutanota.com",           1 },
    { "1password.com",          1 },
    { "bitwarden.com",          1 },
    { "lastpass.com",           1 },

    { "paypal.com",             1 },
    { "paypal.me",              1 },
    { "stripe.com",             1 },

    { "crates.io",              1 },
    { "rust-lang.org",          1 },
    { "python.org",             1 },
    { "pypi.org",               1 },
    { "npmjs.com",              1 },
    { "nodejs.org",             1 },
    { "docker.com",             1 },
    { "hub.docker.com",         1 },
    { "kernel.org",             1 },
    { "gnu.org",                0 },

    { "archlinux.org",          1 },
    { "debian.org",             0 },
    { "fedoraproject.org",      0 },
    { "ubuntu.com",             1 },
    { "opensuse.org",           1 },

    { "nytimes.com",            0 },
    { "bbc.com",                0 },
    { "bbc.co.uk",              0 },
    { "theguardian.com",        0 },
    { "wsj.com",                0 },
    { "reuters.com",            0 },
    { "ft.com",                 0 },
    { "economist.com",          0 },

    { "hackernews.com",         1 },
    { "news.ycombinator.com",   1 },
};

#define ND_HSTS_PRELOAD_COUNT \
    (sizeof(nd_hsts_preload) / sizeof(nd_hsts_preload[0]))

#endif
