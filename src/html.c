/* Nordstjernen — HTML helper utilities shared across the lexbor frontend.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "html.h"

#include <string.h>
#include <uchardet.h>

#include <lexbor/core/base.h>

gboolean
ns_html_is_void(const char *tag)
{
    if (!tag) return FALSE;
    static const char *const voids[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr",
        NULL,
    };
    for (int i = 0; voids[i]; i++)
        if (strcmp(tag, voids[i]) == 0)
            return TRUE;
    return FALSE;
}

gboolean
ns_html_is_raw_text(const char *tag)
{
    if (!tag) return FALSE;
    static const char *const raws[] = {
        "script", "style", "xmp", "iframe", "noembed", "noframes",
        "noscript", "plaintext",
        NULL,
    };
    for (int i = 0; raws[i]; i++)
        if (g_ascii_strcasecmp(tag, raws[i]) == 0)
            return TRUE;
    return FALSE;
}

void
ns_html_escape_append(GString *out, const char *s, gboolean escape_quotes)
{
    for (const char *p = s ? s : ""; *p; p++) {
        switch (*p) {
        case '&': g_string_append(out, "&amp;"); break;
        case '<': g_string_append(out, "&lt;");  break;
        case '>': g_string_append(out, "&gt;");  break;
        case '"':
            if (escape_quotes) g_string_append(out, "&quot;");
            else               g_string_append_c(out, '"');
            break;
        case '\xc2':
            if ((unsigned char)p[1] == 0xa0) {
                g_string_append(out, "&nbsp;");
                p++;
            } else {
                g_string_append_c(out, *p);
            }
            break;
        default:  g_string_append_c(out, *p);    break;
        }
    }
}

char *
ns_html_escape_text(const char *s)
{
    GString *g = g_string_new(NULL);
    ns_html_escape_append(g, s, TRUE);
    return g_string_free(g, FALSE);
}

char *
ns_html_image_document(const char *url)
{
    const char *u = url ? url : "";
    char *esc = ns_html_escape_text(u);
    char *name = g_path_get_basename(u);
    char *query = strchr(name, '?');
    if (query) *query = '\0';
    char *esc_name = ns_html_escape_text(*name ? name : "image");
    char *html = g_strdup_printf(
        "<!DOCTYPE html><html><head><title>%s</title><style>"
        "html,body{margin:0;min-height:100vh}"
        "body{background:#1c1d1e;text-align:center}"
        "img{max-width:100vw;max-height:100vh}"
        "</style></head><body><img src=\"%s\" alt=\"\"></body></html>",
        esc_name, esc);
    g_free(esc);
    g_free(esc_name);
    g_free(name);
    return html;
}

static const char NS_DOC_VIEWER_STYLE[] =
    "<style>"
    "body{margin:0;background:#fbfbfd;color:#1a1a1a;"
    "font:13px/1.55 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}"
    "pre{margin:0;padding:14px;white-space:pre;tab-size:2}"
    ".k{color:#9b2393}.s{color:#1a7f37}.n{color:#0b69c7}.b{color:#b35900}"
    ".p{color:#6e7781}.tag{color:#116329}.at{color:#6f42c1}.av{color:#1a7f37}"
    ".cm{color:#6a737d;font-style:italic}.pi{color:#6e7781}"
    "</style>";

static void
doc_append_escaped(GString *o, const char *s, const char *e)
{
    for (const char *p = s; p < e; p++) {
        switch (*p) {
        case '<': g_string_append(o, "&lt;"); break;
        case '>': g_string_append(o, "&gt;"); break;
        case '&': g_string_append(o, "&amp;"); break;
        default:  g_string_append_c(o, *p);
        }
    }
}

static void
doc_indent(GString *o, int depth)
{
    for (int i = 0; i < depth && i < 64; i++) g_string_append(o, "  ");
}

typedef struct {
    const char *p;
    const char *end;
    GString    *o;
    gboolean    ok;
} json_ctx;

static void
json_ws(json_ctx *c)
{
    while (c->p < c->end &&
           (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r'))
        c->p++;
}

static void
json_string(json_ctx *c, const char *cls)
{
    const char *s = c->p;
    c->p++;
    while (c->p < c->end && *c->p != '"') {
        if (*c->p == '\\' && c->p + 1 < c->end) c->p++;
        c->p++;
    }
    if (c->p >= c->end) { c->ok = FALSE; return; }
    c->p++;
    g_string_append_printf(c->o, "<span class=%s>", cls);
    doc_append_escaped(c->o, s, c->p);
    g_string_append(c->o, "</span>");
}

static gboolean
json_literal(json_ctx *c, const char *lit)
{
    size_t n = strlen(lit);
    if ((size_t)(c->end - c->p) >= n && strncmp(c->p, lit, n) == 0) {
        g_string_append_printf(c->o, "<span class=b>%s</span>", lit);
        c->p += n;
        return TRUE;
    }
    return FALSE;
}

static void json_value(json_ctx *c, int depth);

static void
json_value(json_ctx *c, int depth)
{
    if (depth > 256) { c->ok = FALSE; return; }
    json_ws(c);
    if (c->p >= c->end) { c->ok = FALSE; return; }
    char ch = *c->p;
    if (ch == '{' || ch == '[') {
        char close = ch == '{' ? '}' : ']';
        gboolean obj = ch == '{';
        g_string_append_printf(c->o, "<span class=p>%c</span>", ch);
        c->p++;
        json_ws(c);
        if (c->p < c->end && *c->p == close) {
            c->p++;
            g_string_append_printf(c->o, "<span class=p>%c</span>", close);
            return;
        }
        for (;;) {
            g_string_append_c(c->o, '\n');
            doc_indent(c->o, depth + 1);
            json_ws(c);
            if (obj) {
                if (c->p >= c->end || *c->p != '"') { c->ok = FALSE; return; }
                json_string(c, "k");
                json_ws(c);
                if (c->p >= c->end || *c->p != ':') { c->ok = FALSE; return; }
                c->p++;
                g_string_append(c->o, "<span class=p>: </span>");
            }
            json_value(c, depth + 1);
            if (!c->ok) return;
            json_ws(c);
            if (c->p < c->end && *c->p == ',') {
                c->p++;
                g_string_append(c->o, "<span class=p>,</span>");
                continue;
            }
            break;
        }
        g_string_append_c(c->o, '\n');
        doc_indent(c->o, depth);
        if (c->p >= c->end || *c->p != close) { c->ok = FALSE; return; }
        c->p++;
        g_string_append_printf(c->o, "<span class=p>%c</span>", close);
        return;
    }
    if (ch == '"') { json_string(c, "s"); return; }
    if (ch == '-' || (ch >= '0' && ch <= '9')) {
        const char *s = c->p;
        if (*c->p == '-') c->p++;
        while (c->p < c->end &&
               ((*c->p >= '0' && *c->p <= '9') || *c->p == '.' ||
                *c->p == 'e' || *c->p == 'E' || *c->p == '+' || *c->p == '-'))
            c->p++;
        if (c->p == s) { c->ok = FALSE; return; }
        g_string_append(c->o, "<span class=n>");
        doc_append_escaped(c->o, s, c->p);
        g_string_append(c->o, "</span>");
        return;
    }
    if (json_literal(c, "true") || json_literal(c, "false") ||
        json_literal(c, "null"))
        return;
    c->ok = FALSE;
}

char *
ns_html_json_document(const char *url, const char *json, gsize len)
{
    if (!json) return NULL;
    GString *o = g_string_new(NULL);
    json_ctx c = { json, json + len, o, TRUE };
    json_ws(&c);
    json_value(&c, 0);
    if (!c.ok) { g_string_free(o, TRUE); return NULL; }
    char *esc_url = ns_html_escape_text(url && *url ? url : "");
    char *html = g_strconcat(
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>",
        esc_url, "</title>", NS_DOC_VIEWER_STYLE,
        "</head><body><pre>", o->str, "</pre></body></html>", NULL);
    g_free(esc_url);
    g_string_free(o, TRUE);
    return html;
}

static const char *
xml_tag_end(const char *p, const char *end)
{
    char quote = 0;
    while (p < end) {
        char c = *p;
        if (quote) { if (c == quote) quote = 0; }
        else if (c == '"' || c == '\'') quote = c;
        else if (c == '>') return p;
        p++;
    }
    return NULL;
}

static void
xml_emit_line(GString *o, int depth, const char *cls,
              const char *s, const char *e)
{
    if (o->len) g_string_append_c(o, '\n');
    doc_indent(o, depth);
    if (cls) g_string_append_printf(o, "<span class=%s>", cls);
    doc_append_escaped(o, s, e);
    if (cls) g_string_append(o, "</span>");
}

char *
ns_html_xml_document(const char *url, const char *xml, gsize len)
{
    if (!xml) return NULL;
    GString *o = g_string_new(NULL);
    const char *p = xml, *end = xml + len;
    int depth = 0;
    while (p < end) {
        if (*p != '<') {
            const char *t = p;
            while (p < end && *p != '<') p++;
            const char *ts = t, *te = p;
            while (ts < te && g_ascii_isspace(*ts)) ts++;
            while (te > ts && g_ascii_isspace(*(te - 1))) te--;
            if (te > ts) xml_emit_line(o, depth, NULL, ts, te);
            continue;
        }
        if (end - p >= 4 && strncmp(p, "<!--", 4) == 0) {
            const char *e = g_strstr_len(p, end - p, "-->");
            const char *te = e ? e + 3 : end;
            xml_emit_line(o, depth, "cm", p, te);
            p = te;
            continue;
        }
        if (end - p >= 9 && strncmp(p, "<![CDATA[", 9) == 0) {
            const char *e = g_strstr_len(p, end - p, "]]>");
            const char *te = e ? e + 3 : end;
            xml_emit_line(o, depth, "s", p, te);
            p = te;
            continue;
        }
        if (p + 1 < end && (p[1] == '!' || p[1] == '?')) {
            const char *e = xml_tag_end(p, end);
            const char *te = e ? e + 1 : end;
            xml_emit_line(o, depth, "pi", p, te);
            p = te;
            continue;
        }
        const char *e = xml_tag_end(p, end);
        if (!e) { xml_emit_line(o, depth, "tag", p, end); break; }
        gboolean is_end = (p + 1 < end && p[1] == '/');
        gboolean self_close = (e > p && *(e - 1) == '/');
        if (is_end && depth > 0) depth--;
        xml_emit_line(o, depth, "tag", p, e + 1);
        if (!is_end && !self_close) depth++;
        p = e + 1;
    }
    char *esc_url = ns_html_escape_text(url && *url ? url : "");
    char *html = g_strconcat(
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>",
        esc_url, "</title>", NS_DOC_VIEWER_STYLE,
        "</head><body><pre>", o->str, "</pre></body></html>", NULL);
    g_free(esc_url);
    g_string_free(o, TRUE);
    return html;
}

static char *
charset_normalize(const char *name)
{
    static const struct { const char *label; const char *iconv_name; } map[] = {
        { "gb2312",          "GBK" },
        { "gb_2312-80",      "GBK" },
        { "csgb2312",        "GBK" },
        { "iso-8859-1",      "WINDOWS-1252" },
        { "latin1",          "WINDOWS-1252" },
        { "ascii",           "WINDOWS-1252" },
        { "us-ascii",        "WINDOWS-1252" },
        { "utf8",            "UTF-8" },
        { "big5-hkscs",      "BIG5-HKSCS" },
        { "x-cp1251",        "WINDOWS-1251" },
        { "koi8_r",          "KOI8-R" },
        { "koi",             "KOI8-R" },
        { "x-mac-cyrillic",  "MAC-CYRILLIC" },
        { "x-mac-ukrainian", "MAC-CYRILLIC" },
        { "maccyrillic",     "MAC-CYRILLIC" },
        { "x-cp1252",        "WINDOWS-1252" },
        { "x-cp1250",        "WINDOWS-1250" },
        { "shift_jis",       "CP932" },
        { "shift-jis",       "CP932" },
        { "sjis",            "CP932" },
        { "x-sjis",          "CP932" },
        { "ms_kanji",        "CP932" },
        { "csshiftjis",      "CP932" },
        { "windows-31j",     "CP932" },
        { "x-euc-jp",        "EUC-JP" },
        { "eucjp",           "EUC-JP" },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(map); i++)
        if (g_ascii_strcasecmp(name, map[i].label) == 0)
            return g_strdup(map[i].iconv_name);
    return g_ascii_strup(name, -1);
}

static char *
charset_value_in(const char *s, gsize len)
{
    for (gsize i = 0; i + 7 <= len; i++) {
        if (g_ascii_strncasecmp(s + i, "charset", 7) != 0) continue;
        gsize p = i + 7;
        while (p < len && g_ascii_isspace(s[p])) p++;
        if (p >= len || s[p] != '=') continue;
        p++;
        while (p < len && g_ascii_isspace(s[p])) p++;
        if (p < len && (s[p] == '"' || s[p] == '\'')) p++;
        gsize start = p;
        while (p < len && (g_ascii_isalnum(s[p]) || s[p] == '-' ||
                           s[p] == '_' || s[p] == ':' || s[p] == '.'))
            p++;
        if (p > start && p - start < 40)
            return g_strndup(s + start, p - start);
    }
    return NULL;
}

static void
charset_report(char **charset_out, const char *name)
{
    if (charset_out && !*charset_out) *charset_out = g_strdup(name);
}


static const struct {
    const char *label;
    const char *name;
} ns_encoding_labels[] = {
    { "866", "IBM866" },
    { "ansi_x3.4-1968", "windows-1252" },
    { "arabic", "ISO-8859-6" },
    { "ascii", "windows-1252" },
    { "asmo-708", "ISO-8859-6" },
    { "big5", "Big5" },
    { "big5-hkscs", "Big5" },
    { "chinese", "GBK" },
    { "cn-big5", "Big5" },
    { "cp1250", "windows-1250" },
    { "cp1251", "windows-1251" },
    { "cp1252", "windows-1252" },
    { "cp1253", "windows-1253" },
    { "cp1254", "windows-1254" },
    { "cp1255", "windows-1255" },
    { "cp1256", "windows-1256" },
    { "cp1257", "windows-1257" },
    { "cp1258", "windows-1258" },
    { "cp819", "windows-1252" },
    { "cp866", "IBM866" },
    { "csbig5", "Big5" },
    { "cseuckr", "EUC-KR" },
    { "cseucpkdfmtjapanese", "EUC-JP" },
    { "csgb2312", "GBK" },
    { "csibm866", "IBM866" },
    { "csiso2022jp", "ISO-2022-JP" },
    { "csiso2022kr", "replacement" },
    { "csiso58gb231280", "GBK" },
    { "csiso88596e", "ISO-8859-6" },
    { "csiso88596i", "ISO-8859-6" },
    { "csiso88598e", "ISO-8859-8" },
    { "csiso88598i", "ISO-8859-8-I" },
    { "csisolatin1", "windows-1252" },
    { "csisolatin2", "ISO-8859-2" },
    { "csisolatin3", "ISO-8859-3" },
    { "csisolatin4", "ISO-8859-4" },
    { "csisolatin5", "windows-1254" },
    { "csisolatin6", "ISO-8859-10" },
    { "csisolatin9", "ISO-8859-15" },
    { "csisolatinarabic", "ISO-8859-6" },
    { "csisolatincyrillic", "ISO-8859-5" },
    { "csisolatingreek", "ISO-8859-7" },
    { "csisolatinhebrew", "ISO-8859-8" },
    { "cskoi8r", "KOI8-R" },
    { "csksc56011987", "EUC-KR" },
    { "csmacintosh", "macintosh" },
    { "csshiftjis", "Shift_JIS" },
    { "cyrillic", "ISO-8859-5" },
    { "dos-874", "windows-874" },
    { "ecma-114", "ISO-8859-6" },
    { "ecma-118", "ISO-8859-7" },
    { "elot_928", "ISO-8859-7" },
    { "euc-jp", "EUC-JP" },
    { "euc-kr", "EUC-KR" },
    { "gb18030", "gb18030" },
    { "gb2312", "GBK" },
    { "gb_2312", "GBK" },
    { "gb_2312-80", "GBK" },
    { "gbk", "GBK" },
    { "greek", "ISO-8859-7" },
    { "greek8", "ISO-8859-7" },
    { "hebrew", "ISO-8859-8" },
    { "hz-gb-2312", "replacement" },
    { "ibm819", "windows-1252" },
    { "ibm866", "IBM866" },
    { "iso-2022-cn", "replacement" },
    { "iso-2022-cn-ext", "replacement" },
    { "iso-2022-jp", "ISO-2022-JP" },
    { "iso-2022-kr", "replacement" },
    { "iso-8859-1", "windows-1252" },
    { "iso-8859-10", "ISO-8859-10" },
    { "iso-8859-11", "windows-874" },
    { "iso-8859-13", "ISO-8859-13" },
    { "iso-8859-14", "ISO-8859-14" },
    { "iso-8859-15", "ISO-8859-15" },
    { "iso-8859-16", "ISO-8859-16" },
    { "iso-8859-2", "ISO-8859-2" },
    { "iso-8859-3", "ISO-8859-3" },
    { "iso-8859-4", "ISO-8859-4" },
    { "iso-8859-5", "ISO-8859-5" },
    { "iso-8859-6", "ISO-8859-6" },
    { "iso-8859-6-e", "ISO-8859-6" },
    { "iso-8859-6-i", "ISO-8859-6" },
    { "iso-8859-7", "ISO-8859-7" },
    { "iso-8859-8", "ISO-8859-8" },
    { "iso-8859-8-e", "ISO-8859-8" },
    { "iso-8859-8-i", "ISO-8859-8-I" },
    { "iso-8859-9", "windows-1254" },
    { "iso-ir-100", "windows-1252" },
    { "iso-ir-101", "ISO-8859-2" },
    { "iso-ir-109", "ISO-8859-3" },
    { "iso-ir-110", "ISO-8859-4" },
    { "iso-ir-126", "ISO-8859-7" },
    { "iso-ir-127", "ISO-8859-6" },
    { "iso-ir-138", "ISO-8859-8" },
    { "iso-ir-144", "ISO-8859-5" },
    { "iso-ir-148", "windows-1254" },
    { "iso-ir-149", "EUC-KR" },
    { "iso-ir-157", "ISO-8859-10" },
    { "iso-ir-58", "GBK" },
    { "iso8859-1", "windows-1252" },
    { "iso8859-10", "ISO-8859-10" },
    { "iso8859-11", "windows-874" },
    { "iso8859-13", "ISO-8859-13" },
    { "iso8859-14", "ISO-8859-14" },
    { "iso8859-15", "ISO-8859-15" },
    { "iso8859-2", "ISO-8859-2" },
    { "iso8859-3", "ISO-8859-3" },
    { "iso8859-4", "ISO-8859-4" },
    { "iso8859-5", "ISO-8859-5" },
    { "iso8859-6", "ISO-8859-6" },
    { "iso8859-7", "ISO-8859-7" },
    { "iso8859-8", "ISO-8859-8" },
    { "iso8859-9", "windows-1254" },
    { "iso88591", "windows-1252" },
    { "iso885910", "ISO-8859-10" },
    { "iso885911", "windows-874" },
    { "iso885913", "ISO-8859-13" },
    { "iso885914", "ISO-8859-14" },
    { "iso885915", "ISO-8859-15" },
    { "iso88592", "ISO-8859-2" },
    { "iso88593", "ISO-8859-3" },
    { "iso88594", "ISO-8859-4" },
    { "iso88595", "ISO-8859-5" },
    { "iso88596", "ISO-8859-6" },
    { "iso88597", "ISO-8859-7" },
    { "iso88598", "ISO-8859-8" },
    { "iso88599", "windows-1254" },
    { "iso_8859-1", "windows-1252" },
    { "iso_8859-15", "ISO-8859-15" },
    { "iso_8859-1:1987", "windows-1252" },
    { "iso_8859-2", "ISO-8859-2" },
    { "iso_8859-2:1987", "ISO-8859-2" },
    { "iso_8859-3", "ISO-8859-3" },
    { "iso_8859-3:1988", "ISO-8859-3" },
    { "iso_8859-4", "ISO-8859-4" },
    { "iso_8859-4:1988", "ISO-8859-4" },
    { "iso_8859-5", "ISO-8859-5" },
    { "iso_8859-5:1988", "ISO-8859-5" },
    { "iso_8859-6", "ISO-8859-6" },
    { "iso_8859-6:1987", "ISO-8859-6" },
    { "iso_8859-7", "ISO-8859-7" },
    { "iso_8859-7:1987", "ISO-8859-7" },
    { "iso_8859-8", "ISO-8859-8" },
    { "iso_8859-8:1988", "ISO-8859-8" },
    { "iso_8859-9", "windows-1254" },
    { "iso_8859-9:1989", "windows-1254" },
    { "koi", "KOI8-R" },
    { "koi8", "KOI8-R" },
    { "koi8-r", "KOI8-R" },
    { "koi8-ru", "KOI8-U" },
    { "koi8-u", "KOI8-U" },
    { "koi8_r", "KOI8-R" },
    { "korean", "EUC-KR" },
    { "ks_c_5601-1987", "EUC-KR" },
    { "ks_c_5601-1989", "EUC-KR" },
    { "ksc5601", "EUC-KR" },
    { "ksc_5601", "EUC-KR" },
    { "l1", "windows-1252" },
    { "l2", "ISO-8859-2" },
    { "l3", "ISO-8859-3" },
    { "l4", "ISO-8859-4" },
    { "l5", "windows-1254" },
    { "l6", "ISO-8859-10" },
    { "l9", "ISO-8859-15" },
    { "latin1", "windows-1252" },
    { "latin2", "ISO-8859-2" },
    { "latin3", "ISO-8859-3" },
    { "latin4", "ISO-8859-4" },
    { "latin5", "windows-1254" },
    { "latin6", "ISO-8859-10" },
    { "logical", "ISO-8859-8-I" },
    { "mac", "macintosh" },
    { "macintosh", "macintosh" },
    { "ms932", "Shift_JIS" },
    { "ms_kanji", "Shift_JIS" },
    { "shift-jis", "Shift_JIS" },
    { "shift_jis", "Shift_JIS" },
    { "sjis", "Shift_JIS" },
    { "sun_eu_greek", "ISO-8859-7" },
    { "tis-620", "windows-874" },
    { "unicode-1-1-utf-8", "UTF-8" },
    { "us-ascii", "windows-1252" },
    { "utf-16", "UTF-8" },
    { "utf-16be", "UTF-8" },
    { "utf-16le", "UTF-8" },
    { "utf-8", "UTF-8" },
    { "utf8", "UTF-8" },
    { "visual", "ISO-8859-8" },
    { "windows-1250", "windows-1250" },
    { "windows-1251", "windows-1251" },
    { "windows-1252", "windows-1252" },
    { "windows-1253", "windows-1253" },
    { "windows-1254", "windows-1254" },
    { "windows-1255", "windows-1255" },
    { "windows-1256", "windows-1256" },
    { "windows-1257", "windows-1257" },
    { "windows-1258", "windows-1258" },
    { "windows-31j", "Shift_JIS" },
    { "windows-874", "windows-874" },
    { "windows-949", "EUC-KR" },
    { "x-cp1250", "windows-1250" },
    { "x-cp1251", "windows-1251" },
    { "x-cp1252", "windows-1252" },
    { "x-cp1253", "windows-1253" },
    { "x-cp1254", "windows-1254" },
    { "x-cp1255", "windows-1255" },
    { "x-cp1256", "windows-1256" },
    { "x-cp1257", "windows-1257" },
    { "x-cp1258", "windows-1258" },
    { "x-euc-jp", "EUC-JP" },
    { "x-gbk", "GBK" },
    { "x-mac-cyrillic", "x-mac-cyrillic" },
    { "x-mac-roman", "macintosh" },
    { "x-mac-ukrainian", "x-mac-cyrillic" },
    { "x-sjis", "Shift_JIS" },
    { "x-user-defined", "windows-1252" },
    { "x-x-big5", "Big5" },
};

static const char *
ns_encoding_label_to_name(const char *label)
{
    if (!label) return NULL;
    while (*label == ' ' || *label == '\t' || *label == '\n' ||
           *label == '\f' || *label == '\r')
        label++;
    gsize len = strlen(label);
    while (len > 0 && (label[len - 1] == ' ' || label[len - 1] == '\t' ||
                       label[len - 1] == '\n' || label[len - 1] == '\f' ||
                       label[len - 1] == '\r'))
        len--;
    for (gsize i = 0; i < G_N_ELEMENTS(ns_encoding_labels); i++) {
        if (strlen(ns_encoding_labels[i].label) == len &&
            g_ascii_strncasecmp(ns_encoding_labels[i].label, label, len) == 0)
            return ns_encoding_labels[i].name;
    }
    return NULL;
}

char *
ns_html_declared_charset(const char *body, gsize len, const char *content_type)
{
    char *label = content_type
        ? charset_value_in(content_type, strlen(content_type)) : NULL;
    gboolean from_meta = FALSE;
    if (!label && body) {
        label = charset_value_in(body, len < 1024 ? len : 1024);
        from_meta = label != NULL;
    }
    if (!label) return NULL;
    const char *name = ns_encoding_label_to_name(label);
    g_free(label);
    if (!name) return NULL;
    if (from_meta && g_str_has_prefix(name, "UTF-16"))
        name = "UTF-8";
    return g_strdup(name);
}

static gboolean
charset_is_dangerous(const char *cs)
{
    if (!cs || !*cs) return TRUE;
    char *up = g_ascii_strup(cs, -1);
    gboolean bad =
        strstr(up, "UTF-7") || strstr(up, "UTF7") ||
        strstr(up, "REPLACEMENT") ||
        g_str_has_prefix(up, "HZ") ||
        strstr(up, "2022-CN") || strstr(up, "2022CN") ||
        strstr(up, "2022-KR") || strstr(up, "2022KR") ||
        strstr(up, "IMAP") ||
        strstr(up, "CESU") || strstr(up, "BOCU") || strstr(up, "SCSU");
    g_free(up);
    return bad;
}

char *
ns_html_decode_body_full(const char *body, gsize len,
                         const char *content_type, char **charset_out)
{
    if (charset_out) *charset_out = NULL;
    if (!body || len == 0) return g_strdup("");

    if (len >= 3 && memcmp(body, "\xef\xbb\xbf", 3) == 0) {
        charset_report(charset_out, "UTF-8");
        return g_utf8_make_valid(body + 3, (gssize)(len - 3));
    }
    if (len >= 2 && memcmp(body, "\xff\xfe", 2) == 0) {
        char *out = g_convert(body + 2, (gssize)(len - 2), "UTF-8", "UTF-16LE",
                              NULL, NULL, NULL);
        if (out) {
            charset_report(charset_out, "UTF-16LE");
            return out;
        }
    }
    if (len >= 2 && memcmp(body, "\xfe\xff", 2) == 0) {
        char *out = g_convert(body + 2, (gssize)(len - 2), "UTF-8", "UTF-16BE",
                              NULL, NULL, NULL);
        if (out) {
            charset_report(charset_out, "UTF-16BE");
            return out;
        }
    }

    char *declared = content_type
        ? charset_value_in(content_type, strlen(content_type)) : NULL;
    if (!declared)
        declared = charset_value_in(body, len < 1024 ? len : 1024);
    gboolean declared_utf8 = FALSE;
    if (declared) {
        char *cs = charset_normalize(declared);
        g_free(declared);
        if (g_ascii_strcasecmp(cs, "UTF-8") == 0) {
            declared_utf8 = TRUE;
        } else if (!charset_is_dangerous(cs)) {
            char *out = g_convert(body, (gssize)len, "UTF-8", cs,
                                  NULL, NULL, NULL);
            if (out) {
                charset_report(charset_out, cs);
                g_free(cs);
                return out;
            }
        }
        g_free(cs);
    }

    if (g_utf8_validate(body, (gssize)len, NULL)) {
        charset_report(charset_out, "UTF-8");
        return g_strndup(body, len);
    }

    if (declared_utf8) {
        charset_report(charset_out, "UTF-8");
        return g_utf8_make_valid(body, (gssize)len);
    }

    char *charset = NULL;
    uchardet_t det = uchardet_new();
    if (det) {
        gsize scan = len < (gsize)1024 * 1024 ? len : (gsize)1024 * 1024;
        if (uchardet_handle_data(det, body, scan) == 0) {
            uchardet_data_end(det);
            const char *name = uchardet_get_charset(det);
            if (name && *name
                && g_ascii_strcasecmp(name, "ASCII") != 0
                && g_ascii_strcasecmp(name, "UTF-8") != 0)
                charset = g_strdup(name);
        }
        uchardet_delete(det);
    }

    if (charset) {
        if (!charset_is_dangerous(charset)) {
            char *out = g_convert(body, (gssize)len, "UTF-8", charset,
                                  NULL, NULL, NULL);
            if (out) {
                charset_report(charset_out, charset);
                g_free(charset);
                return out;
            }
        }
        g_free(charset);
    }

    char *latin1 = g_convert(body, (gssize)len, "UTF-8", "WINDOWS-1252",
                             NULL, NULL, NULL);
    if (latin1) {
        charset_report(charset_out, "WINDOWS-1252");
        return latin1;
    }

    charset_report(charset_out, "UTF-8");
    return g_utf8_make_valid(body, (gssize)len);
}

