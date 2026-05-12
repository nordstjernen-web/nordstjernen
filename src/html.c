/* Nordstjernen — pragmatic HTML parser. */

#include "html.h"

#include <ctype.h>
#include <string.h>

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

static void
append_codepoint(GString *out, guint32 cp)
{

    char buf[6];
    int n = g_unichar_to_utf8((gunichar)cp, buf);
    if (n > 0)
        g_string_append_len(out, buf, n);
}

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

    struct { const char *name; const char *expansion; } named[] = {
        { "amp",     "&"  },
        { "lt",      "<"  },
        { "gt",      ">"  },
        { "quot",    "\"" },
        { "apos",    "'"  },
        { "nbsp",    "\xc2\xa0" },
        { "copy",    "\xc2\xa9" },
        { "reg",     "\xc2\xae" },
        { "trade",   "\xe2\x84\xa2" },
        { "mdash",   "\xe2\x80\x94" },
        { "ndash",   "\xe2\x80\x93" },
        { "hellip",  "\xe2\x80\xa6" },
        { "lsquo",   "\xe2\x80\x98" },
        { "rsquo",   "\xe2\x80\x99" },
        { "ldquo",   "\xe2\x80\x9c" },
        { "rdquo",   "\xe2\x80\x9d" },
        { "bull",    "\xe2\x80\xa2" },
        { "middot",  "\xc2\xb7" },
        { "laquo",   "\xc2\xab" },
        { "raquo",   "\xc2\xbb" },
        { "lsaquo",  "\xe2\x80\xb9" },
        { "rsaquo",  "\xe2\x80\xba" },
        { "deg",     "\xc2\xb0" },
        { "plusmn",  "\xc2\xb1" },
        { "times",   "\xc3\x97" },
        { "divide",  "\xc3\xb7" },
        { "minus",   "\xe2\x88\x92" },
        { "sect",    "\xc2\xa7" },
        { "para",    "\xc2\xb6" },
        { "dagger",  "\xe2\x80\xa0" },
        { "Dagger",  "\xe2\x80\xa1" },
        { "permil",  "\xe2\x80\xb0" },
        { "larr",    "\xe2\x86\x90" },
        { "rarr",    "\xe2\x86\x92" },
        { "uarr",    "\xe2\x86\x91" },
        { "darr",    "\xe2\x86\x93" },
        { "harr",    "\xe2\x86\x94" },
        { "iexcl",   "\xc2\xa1" },
        { "iquest",  "\xc2\xbf" },
        { "cent",    "\xc2\xa2" },
        { "pound",   "\xc2\xa3" },
        { "yen",     "\xc2\xa5" },
        { "euro",    "\xe2\x82\xac" },
        { "curren",  "\xc2\xa4" },
        { "brvbar",  "\xc2\xa6" },
        { "uml",     "\xc2\xa8" },
        { "ordf",    "\xc2\xaa" },
        { "not",     "\xc2\xac" },
        { "shy",     "\xc2\xad" },
        { "macr",    "\xc2\xaf" },
        { "sup1",    "\xc2\xb9" },
        { "sup2",    "\xc2\xb2" },
        { "sup3",    "\xc2\xb3" },
        { "acute",   "\xc2\xb4" },
        { "micro",   "\xc2\xb5" },
        { "cedil",   "\xc2\xb8" },
        { "ordm",    "\xc2\xba" },
        { "frac14",  "\xc2\xbc" },
        { "frac12",  "\xc2\xbd" },
        { "frac34",  "\xc2\xbe" },
        { "Auml",    "\xc3\x84" }, { "auml",    "\xc3\xa4" },
        { "Ouml",    "\xc3\x96" }, { "ouml",    "\xc3\xb6" },
        { "Uuml",    "\xc3\x9c" }, { "uuml",    "\xc3\xbc" },
        { "szlig",   "\xc3\x9f" },
        { "AElig",   "\xc3\x86" }, { "aelig",   "\xc3\xa6" },
        { "Oslash",  "\xc3\x98" }, { "oslash",  "\xc3\xb8" },
        { "Aring",   "\xc3\x85" }, { "aring",   "\xc3\xa5" },
        { "Ccedil",  "\xc3\x87" }, { "ccedil",  "\xc3\xa7" },
        { "Eacute",  "\xc3\x89" }, { "eacute",  "\xc3\xa9" },
        { "Egrave",  "\xc3\x88" }, { "egrave",  "\xc3\xa8" },
        { "Ecirc",   "\xc3\x8a" }, { "ecirc",   "\xc3\xaa" },
        { "Iacute",  "\xc3\x8d" }, { "iacute",  "\xc3\xad" },
        { "Igrave",  "\xc3\x8c" }, { "igrave",  "\xc3\xac" },
        { "Oacute",  "\xc3\x93" }, { "oacute",  "\xc3\xb3" },
        { "Uacute",  "\xc3\x9a" }, { "uacute",  "\xc3\xba" },
        { "Ugrave",  "\xc3\x99" }, { "ugrave",  "\xc3\xb9" },
        { "Ntilde",  "\xc3\x91" }, { "ntilde",  "\xc3\xb1" },
        { "Aacute",  "\xc3\x81" }, { "aacute",  "\xc3\xa1" },
        { "Agrave",  "\xc3\x80" }, { "agrave",  "\xc3\xa0" },
        { "Acirc",   "\xc3\x82" }, { "acirc",   "\xc3\xa2" },
        { "infin",   "\xe2\x88\x9e" },
        { "asymp",   "\xe2\x89\x88" },
        { "ne",      "\xe2\x89\xa0" },
        { "le",      "\xe2\x89\xa4" },
        { "ge",      "\xe2\x89\xa5" },
        { "alpha",   "\xce\xb1" },
        { "beta",    "\xce\xb2" },
        { "gamma",   "\xce\xb3" },
        { "delta",   "\xce\xb4" },
        { "Delta",   "\xce\x94" },
        { "epsilon", "\xce\xb5" },
        { "lambda",  "\xce\xbb" },
        { "Lambda",  "\xce\x9b" },
        { "mu",      "\xce\xbc" },
        { "pi",      "\xcf\x80" },
        { "Pi",      "\xce\xa0" },
        { "sigma",   "\xcf\x83" },
        { "Sigma",   "\xce\xa3" },
        { "tau",     "\xcf\x84" },
        { "phi",     "\xcf\x86" },
        { "Phi",     "\xce\xa6" },
        { "omega",   "\xcf\x89" },
        { "Omega",   "\xce\xa9" },
        { NULL, NULL },
    };

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

typedef struct nd_parser {
    const char *p;
    const char *end;

    GPtrArray *open;
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

static gboolean
pop_until_tag(nd_parser *parser, const char *tag)
{
    int n = (int)parser->open->len;
    int target = -1;
    for (int i = n - 1; i >= 0; i--) {
        nd_node *e = g_ptr_array_index(parser->open, i);
        if (e->kind != ND_NODE_ELEMENT) continue;
        if (strcmp(e->name, tag) == 0) { target = i; break; }

        if (strcmp(e->name, "html") == 0 || strcmp(e->name, "body") == 0 ||
            strcmp(e->name, "head") == 0 || strcmp(e->name, "table") == 0 ||
            strcmp(e->name, "td") == 0   || strcmp(e->name, "th") == 0)
            break;
    }
    if (target < 0) return FALSE;
    while ((int)parser->open->len > target) (void)pop(parser);
    return TRUE;
}

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

            continue;
        }

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
                    if (parser->p < parser->end) parser->p++;
                    aval = g_string_free(v, FALSE);
                } else {

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
nd_html_parse_for_page(const char *input, gssize len)
{
    const char *which = g_getenv("ND_HTML_PARSER");
    if (which && g_ascii_strcasecmp(which, "gumbo") == 0 &&
        nd_html_gumbo_available()) {
        nd_node *doc = nd_html_parse_gumbo(input, len);
        if (doc) return doc;
    }
    return nd_html_parse(input, len);
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

                    while (parser.p < parser.end && *parser.p != '>') parser.p++;
                    if (parser.p < parser.end) parser.p++;
                }
                continue;
            }

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
                nd_node *el = nd_node_new_element(name);
                gboolean self_closing = parse_attributes(&parser, el);

                if (implicitly_closes_p(el->name))
                    close_p_if_open(&parser);

                nd_node *parent = top(&parser);
                if (!parent) parent = doc;
                nd_node_append_child(parent, el);

                gboolean is_void = is_void_element(el->name);
                gboolean is_raw  = is_rawtext_element(el->name);

                if (is_void || self_closing) {

                } else if (is_raw) {
                    push(&parser, el);
                    consume_rawtext(&parser, el, el->name);

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
