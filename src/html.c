/*
 * Nordstjernen — html.c
 *
 * A small, single-pass HTML parser. Design choices, deliberate:
 *
 *  - No full HTML5 state machine. We collapse the spec's many states
 *    into a handful of branches: data, tag-open, comment, doctype,
 *    rawtext (for script/style). This is "pragmatic" parsing — it
 *    handles well-formed pages, falls back gracefully on garbage, but
 *    will diverge from a spec-compliant parser on adversarial input.
 *
 *  - No active-formatting-elements list, no foster parenting, no
 *    "in body" / "in table" mode tracking. The tree builder is a
 *    plain stack with void-element auto-close and some scope rules
 *    for end tags.
 *
 *  - Entity decoding covers the small set that matters for HTML body
 *    text (named: &amp; &lt; &gt; &quot; &apos; &nbsp;; numeric: &#xNN; &#NN;).
 *    Everything else passes through verbatim.
 *
 * Phase 3+ will exercise the resulting tree; we'll patch holes as they
 * show up against real pages.
 */

#include "html.h"

#include <ctype.h>
#include <string.h>

/* ---------- void elements + rawtext detection ---------- */

static gboolean
is_void_element(const char *name)
{
    static const char *voids[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr",
        NULL,
    };
    for (int i = 0; voids[i]; i++)
        if (strcmp(name, voids[i]) == 0)
            return TRUE;
    return FALSE;
}

static gboolean
is_rawtext_element(const char *name)
{
    return strcmp(name, "script") == 0 ||
           strcmp(name, "style")  == 0 ||
           strcmp(name, "xmp")    == 0 ||
           strcmp(name, "iframe") == 0 ||
           strcmp(name, "noembed") == 0 ||
           strcmp(name, "noframes") == 0;
}

/* End tags for these elements implicitly close any matching open
 * <p> when they open. Keeps paragraph-heavy HTML usable. */
static gboolean
implicitly_closes_p(const char *name)
{
    static const char *list[] = {
        "address","article","aside","blockquote","details","div",
        "dl","fieldset","figcaption","figure","footer","form","h1",
        "h2","h3","h4","h5","h6","header","hgroup","hr","main","menu",
        "nav","ol","p","pre","section","table","ul",
        NULL,
    };
    for (int i = 0; list[i]; i++)
        if (strcmp(name, list[i]) == 0) return TRUE;
    return FALSE;
}

/* ---------- entity decoding ---------- */

static void
append_codepoint(GString *out, guint32 cp)
{
    /* UTF-8 encode */
    char buf[6];
    int n = g_unichar_to_utf8((gunichar)cp, buf);
    if (n > 0)
        g_string_append_len(out, buf, n);
}

/*
 * Try to decode an entity beginning at &. Advances *p past the entity
 * and returns TRUE on success. Returns FALSE without consuming if the
 * entity is unrecognized; the caller should then emit '&' literally
 * and continue.
 */
static gboolean
decode_entity(const char **p, const char *end, GString *out)
{
    const char *s = *p;
    if (s >= end || *s != '&') return FALSE;
    const char *q = s + 1;
    if (q >= end) return FALSE;

    if (*q == '#') {
        q++;
        if (q >= end) return FALSE;
        gboolean hex = (*q == 'x' || *q == 'X');
        if (hex) q++;
        guint32 cp = 0;
        const char *start = q;
        while (q < end) {
            char c = *q;
            int d = -1;
            if (hex) {
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            } else {
                if (c >= '0' && c <= '9') d = c - '0';
            }
            if (d < 0) break;
            cp = cp * (hex ? 16 : 10) + (guint32)d;
            q++;
        }
        if (q == start) return FALSE;
        if (q < end && *q == ';') q++;
        append_codepoint(out, cp);
        *p = q;
        return TRUE;
    }

    /* Named entities. Cheap and small set. */
    struct { const char *name; const char *expansion; } named[] = {
        { "amp",   "&"  },
        { "lt",    "<"  },
        { "gt",    ">"  },
        { "quot",  "\"" },
        { "apos",  "'"  },
        { "nbsp",  "\xc2\xa0" },
        { "copy",  "\xc2\xa9" },
        { "reg",   "\xc2\xae" },
        { "mdash", "\xe2\x80\x94" },
        { "ndash", "\xe2\x80\x93" },
        { "hellip","\xe2\x80\xa6" },
        { "lsquo", "\xe2\x80\x98" },
        { "rsquo", "\xe2\x80\x99" },
        { "ldquo", "\xe2\x80\x9c" },
        { "rdquo", "\xe2\x80\x9d" },
        { "trade", "\xe2\x84\xa2" },
        { NULL, NULL },
    };
    /* Read name (letters/digits) */
    const char *nstart = q;
    while (q < end && g_ascii_isalnum(*q)) q++;
    gsize nlen = (gsize)(q - nstart);
    if (nlen == 0) return FALSE;
    gboolean had_semi = (q < end && *q == ';');
    for (int i = 0; named[i].name; i++) {
        if (strlen(named[i].name) == nlen &&
            strncmp(named[i].name, nstart, nlen) == 0) {
            g_string_append(out, named[i].expansion);
            if (had_semi) q++;
            *p = q;
            return TRUE;
        }
    }
    return FALSE;
}

/* ---------- tokenizer ---------- */

typedef struct nd_parser {
    const char *p;
    const char *end;
    /* Stack of open elements (parents-first). The bottom of the stack
     * is the document node. */
    GPtrArray *open; /* nd_node* */
} nd_parser;

static nd_node *
top(nd_parser *parser)
{
    if (parser->open->len == 0) return NULL;
    return g_ptr_array_index(parser->open, parser->open->len - 1);
}

static void
push(nd_parser *parser, nd_node *node)
{
    g_ptr_array_add(parser->open, node);
}

static nd_node *
pop(nd_parser *parser)
{
    if (parser->open->len == 0) return NULL;
    nd_node *n = g_ptr_array_index(parser->open, parser->open->len - 1);
    g_ptr_array_set_size(parser->open, parser->open->len - 1);
    return n;
}

/* Pop until we've removed an element with the given tag name, or
 * we hit a "scope boundary". Conservatively, stop at <html>, <body>,
 * <head>, <table>, <td>, <th>. If no match is found before a
 * boundary, restore the stack (do nothing). */
static gboolean
pop_until_tag(nd_parser *parser, const char *tag)
{
    int n = (int)parser->open->len;
    int target = -1;
    for (int i = n - 1; i >= 0; i--) {
        nd_node *e = g_ptr_array_index(parser->open, i);
        if (e->kind != ND_NODE_ELEMENT) continue;
        if (strcmp(e->name, tag) == 0) { target = i; break; }
        /* scope boundaries */
        if (strcmp(e->name, "html") == 0 || strcmp(e->name, "body") == 0 ||
            strcmp(e->name, "head") == 0 || strcmp(e->name, "table") == 0 ||
            strcmp(e->name, "td") == 0   || strcmp(e->name, "th") == 0)
            break;
    }
    if (target < 0) return FALSE;
    while ((int)parser->open->len > target) (void)pop(parser);
    return TRUE;
}

/* Pop all open <p> elements in scope. */
static void
close_p_if_open(nd_parser *parser)
{
    for (int i = (int)parser->open->len - 1; i >= 0; i--) {
        nd_node *e = g_ptr_array_index(parser->open, i);
        if (e->kind != ND_NODE_ELEMENT) continue;
        if (strcmp(e->name, "p") == 0) {
            while ((int)parser->open->len > i) (void)pop(parser);
            return;
        }
        if (strcmp(e->name, "html") == 0 || strcmp(e->name, "body") == 0 ||
            strcmp(e->name, "table") == 0 || strcmp(e->name, "td") == 0 ||
            strcmp(e->name, "th") == 0)
            return;
    }
}

/* Lowercase ASCII copy. */
static char *
ascii_lower_dup(const char *s, gsize len)
{
    char *r = g_malloc(len + 1);
    for (gsize i = 0; i < len; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        r[i] = c;
    }
    r[len] = '\0';
    return r;
}

static gboolean
is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/* Consume run of attributes after a tag name. Stops at '>' or '/>'.
 * Returns TRUE if self-closing ("/>") was seen. */
static gboolean
parse_attributes(nd_parser *parser, nd_node *el)
{
    gboolean self_closing = FALSE;
    while (parser->p < parser->end) {
        while (parser->p < parser->end && is_ws(*parser->p)) parser->p++;
        if (parser->p >= parser->end) break;
        char c = *parser->p;
        if (c == '>') { parser->p++; break; }
        if (c == '/' ) {
            parser->p++;
            if (parser->p < parser->end && *parser->p == '>') {
                self_closing = TRUE;
                parser->p++;
                break;
            }
            /* lone '/' inside a tag — ignore and continue */
            continue;
        }
        /* attribute name */
        const char *nstart = parser->p;
        while (parser->p < parser->end && !is_ws(*parser->p) &&
               *parser->p != '=' && *parser->p != '>' && *parser->p != '/') {
            parser->p++;
        }
        gsize nlen = (gsize)(parser->p - nstart);
        if (nlen == 0) { parser->p++; continue; }
        char *aname = ascii_lower_dup(nstart, nlen);
        char *aval  = NULL;

        while (parser->p < parser->end && is_ws(*parser->p)) parser->p++;
        if (parser->p < parser->end && *parser->p == '=') {
            parser->p++;
            while (parser->p < parser->end && is_ws(*parser->p)) parser->p++;
            if (parser->p < parser->end) {
                char q = *parser->p;
                if (q == '"' || q == '\'') {
                    parser->p++;
                    GString *v = g_string_new(NULL);
                    while (parser->p < parser->end && *parser->p != q) {
                        if (*parser->p == '&') {
                            const char *save = parser->p;
                            if (!decode_entity(&parser->p, parser->end, v)) {
                                g_string_append_c(v, '&');
                                parser->p = save + 1;
                            }
                        } else {
                            g_string_append_c(v, *parser->p++);
                        }
                    }
                    if (parser->p < parser->end) parser->p++; /* closing quote */
                    aval = g_string_free(v, FALSE);
                } else {
                    /* unquoted */
                    const char *vstart = parser->p;
                    while (parser->p < parser->end && !is_ws(*parser->p) && *parser->p != '>')
                        parser->p++;
                    aval = g_strndup(vstart, (gsize)(parser->p - vstart));
                }
            }
        }
        nd_element_set_attr(el, aname, aval ? aval : "");
        g_free(aname);
        g_free(aval);
    }
    return self_closing;
}

/* Emit accumulated text node, if non-empty. */
static void
emit_text(nd_parser *parser, GString **buf)
{
    if (!*buf || (*buf)->len == 0) {
        if (*buf) g_string_set_size(*buf, 0);
        return;
    }
    nd_node *parent = top(parser);
    if (parent) {
        char *txt = g_string_free(*buf, FALSE);
        nd_node_append_child(parent, nd_node_new_text(txt));
    } else {
        g_string_free(*buf, TRUE);
    }
    *buf = g_string_new(NULL);
}

/* Read a rawtext run until </tag>. Body becomes a single TEXT child of
 * the rawtext element; the element is then closed. */
static void
consume_rawtext(nd_parser *parser, nd_node *el, const char *tag)
{
    GString *body = g_string_new(NULL);
    gsize tlen = strlen(tag);
    while (parser->p < parser->end) {
        if (*parser->p == '<' && parser->p + 1 < parser->end &&
            parser->p[1] == '/' &&
            (gsize)(parser->end - parser->p - 2) >= tlen &&
            g_ascii_strncasecmp(parser->p + 2, tag, tlen) == 0) {
            const char *after = parser->p + 2 + tlen;
            if (after >= parser->end ||
                is_ws(*after) || *after == '>' || *after == '/') {
                /* end tag of the rawtext element */
                parser->p = after;
                while (parser->p < parser->end && *parser->p != '>') parser->p++;
                if (parser->p < parser->end) parser->p++;
                if (body->len > 0)
                    nd_node_append_child(el, nd_node_new_text(g_string_free(body, FALSE)));
                else
                    g_string_free(body, TRUE);
                return;
            }
        }
        g_string_append_c(body, *parser->p++);
    }
    if (body->len > 0)
        nd_node_append_child(el, nd_node_new_text(g_string_free(body, FALSE)));
    else
        g_string_free(body, TRUE);
}

static void
consume_comment(nd_parser *parser, nd_node *parent)
{
    /* parser->p points just past "<!--". Read until "-->" or EOF. */
    GString *body = g_string_new(NULL);
    while (parser->p < parser->end) {
        if (parser->p + 2 < parser->end &&
            parser->p[0] == '-' && parser->p[1] == '-' && parser->p[2] == '>') {
            parser->p += 3;
            break;
        }
        g_string_append_c(body, *parser->p++);
    }
    nd_node_append_child(parent, nd_node_new_comment(g_string_free(body, FALSE)));
}

static void
consume_doctype(nd_parser *parser, nd_node *document)
{
    /* parser->p points just past "<!DOCTYPE". Skip whitespace, read
     * an ident, then skip to '>'. */
    while (parser->p < parser->end && is_ws(*parser->p)) parser->p++;
    const char *nstart = parser->p;
    while (parser->p < parser->end && !is_ws(*parser->p) && *parser->p != '>')
        parser->p++;
    char *name = ascii_lower_dup(nstart, (gsize)(parser->p - nstart));
    while (parser->p < parser->end && *parser->p != '>') parser->p++;
    if (parser->p < parser->end) parser->p++;
    nd_node_append_child(document, nd_node_new_doctype(name));
}

nd_node *
nd_html_parse(const char *input, gssize len_in)
{
    if (len_in < 0) len_in = (gssize)(input ? strlen(input) : 0);
    nd_node *doc = nd_node_new_document();
    if (!input || len_in == 0) return doc;

    nd_parser parser = {
        .p = input,
        .end = input + len_in,
        .open = g_ptr_array_new(),
    };
    push(&parser, doc);

    GString *text = g_string_new(NULL);

    while (parser.p < parser.end) {
        char c = *parser.p;

        if (c == '<' && parser.p + 1 < parser.end) {
            char n1 = parser.p[1];

            /* Comment / doctype / CDATA-ish */
            if (n1 == '!') {
                emit_text(&parser, &text);
                if (parser.p + 3 < parser.end &&
                    parser.p[2] == '-' && parser.p[3] == '-') {
                    parser.p += 4;
                    consume_comment(&parser, top(&parser) ? top(&parser) : doc);
                } else if ((gsize)(parser.end - parser.p) >= 9 &&
                           g_ascii_strncasecmp(parser.p + 2, "DOCTYPE", 7) == 0) {
                    parser.p += 9;
                    consume_doctype(&parser, doc);
                } else {
                    /* Unknown <!...>; skip to '>' */
                    while (parser.p < parser.end && *parser.p != '>') parser.p++;
                    if (parser.p < parser.end) parser.p++;
                }
                continue;
            }

            /* End tag */
            if (n1 == '/') {
                emit_text(&parser, &text);
                parser.p += 2;
                const char *nstart = parser.p;
                while (parser.p < parser.end && !is_ws(*parser.p) &&
                       *parser.p != '>' && *parser.p != '/')
                    parser.p++;
                gsize nlen = (gsize)(parser.p - nstart);
                while (parser.p < parser.end && *parser.p != '>') parser.p++;
                if (parser.p < parser.end) parser.p++;
                if (nlen == 0) continue;
                char *name = ascii_lower_dup(nstart, nlen);
                pop_until_tag(&parser, name);
                g_free(name);
                continue;
            }

            /* Start tag */
            if (g_ascii_isalpha(n1)) {
                emit_text(&parser, &text);
                parser.p += 1;
                const char *nstart = parser.p;
                while (parser.p < parser.end && !is_ws(*parser.p) &&
                       *parser.p != '>' && *parser.p != '/')
                    parser.p++;
                gsize nlen = (gsize)(parser.p - nstart);
                if (nlen == 0) continue;
                char *name = ascii_lower_dup(nstart, nlen);
                nd_node *el = nd_node_new_element(name); /* takes ownership */
                gboolean self_closing = parse_attributes(&parser, el);

                if (implicitly_closes_p(el->name))
                    close_p_if_open(&parser);

                nd_node *parent = top(&parser);
                if (!parent) parent = doc;
                nd_node_append_child(parent, el);

                gboolean is_void = is_void_element(el->name);
                gboolean is_raw  = is_rawtext_element(el->name);

                if (is_void || self_closing) {
                    /* nothing more */
                } else if (is_raw) {
                    push(&parser, el);
                    consume_rawtext(&parser, el, el->name);
                    /* close el */
                    int idx = -1;
                    for (int i = (int)parser.open->len - 1; i >= 0; i--) {
                        if (g_ptr_array_index(parser.open, i) == el) { idx = i; break; }
                    }
                    if (idx >= 0)
                        while ((int)parser.open->len > idx) (void)pop(&parser);
                } else {
                    push(&parser, el);
                }
                continue;
            }

            /* '<' followed by something else — treat as text. */
            g_string_append_c(text, '<');
            parser.p++;
            continue;
        }

        if (c == '&') {
            if (!decode_entity(&parser.p, parser.end, text)) {
                g_string_append_c(text, '&');
                parser.p++;
            }
            continue;
        }

        g_string_append_c(text, c);
        parser.p++;
    }

    emit_text(&parser, &text);
    if (text) g_string_free(text, TRUE);
    g_ptr_array_free(parser.open, TRUE);
    return doc;
}
