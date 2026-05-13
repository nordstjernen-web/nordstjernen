/* Nordstjernen — HTML parser dispatcher; lexbor default when built in, gumbo fallback. */

#include "html.h"

#include <gumbo.h>
#include <stdlib.h>
#include <string.h>
#include <uchardet/uchardet.h>

#include "compatibility.h"

gboolean
nd_html_is_void(const char *tag)
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

static char *
gumbo_tag_name_dup(const GumboElement *el)
{
    if (el->tag != GUMBO_TAG_UNKNOWN) {
        const char *t = gumbo_normalized_tagname(el->tag);
        if (t && *t) return g_ascii_strdown(t, -1);
    }
    if (el->original_tag.data && el->original_tag.length > 0) {
        GumboStringPiece sp = el->original_tag;
        gumbo_tag_from_original_text(&sp);
        if (sp.length > 0)
            return g_ascii_strdown(sp.data, (gssize)sp.length);
    }
    return g_strdup("unknown");
}

static nd_node *
gumbo_convert_self(const GumboNode *gn)
{
    if (!gn) return NULL;
    switch (gn->type) {
    case GUMBO_NODE_DOCUMENT:
        return nd_node_new_document();
    case GUMBO_NODE_ELEMENT:
    case GUMBO_NODE_TEMPLATE: {
        char *name = gumbo_tag_name_dup(&gn->v.element);
        nd_node *el = nd_node_new_element(name);
        const GumboVector *attrs = &gn->v.element.attributes;
        for (unsigned i = 0; i < attrs->length; i++) {
            const GumboAttribute *a = (const GumboAttribute *)attrs->data[i];
            if (a->name && a->value)
                nd_element_set_attr(el, a->name, a->value);
        }
        return el;
    }
    case GUMBO_NODE_TEXT:
    case GUMBO_NODE_WHITESPACE:
    case GUMBO_NODE_CDATA:
        return nd_node_new_text(g_strdup(gn->v.text.text ? gn->v.text.text : ""));
    case GUMBO_NODE_COMMENT:
        return nd_node_new_comment(g_strdup(gn->v.text.text ? gn->v.text.text : ""));
    }
    return NULL;
}

static const GumboVector *
gumbo_children_vector(const GumboNode *gn)
{
    if (!gn) return NULL;
    switch (gn->type) {
    case GUMBO_NODE_DOCUMENT:
        return &gn->v.document.children;
    case GUMBO_NODE_ELEMENT:
    case GUMBO_NODE_TEMPLATE:
        return &gn->v.element.children;
    default:
        return NULL;
    }
}

typedef struct gumbo_walk_frame {
    const GumboNode *src_parent;
    unsigned         next_index;
    nd_node         *nd_parent;
} gumbo_walk_frame;

static void
gumbo_walk_push(GQueue *stack, const GumboNode *parent, nd_node *nd_parent)
{
    const GumboVector *kids = gumbo_children_vector(parent);
    if (!kids || kids->length == 0) return;
    gumbo_walk_frame *fr = g_new(gumbo_walk_frame, 1);
    fr->src_parent = parent;
    fr->next_index = 0;
    fr->nd_parent = nd_parent;
    g_queue_push_head(stack, fr);
}

static nd_node *
gumbo_to_nd(const GumboNode *root)
{
    nd_node *root_nd = gumbo_convert_self(root);
    if (!root_nd) return NULL;
    GQueue stack = G_QUEUE_INIT;
    gumbo_walk_push(&stack, root, root_nd);
    while (!g_queue_is_empty(&stack)) {
        gumbo_walk_frame *fr = g_queue_peek_head(&stack);
        const GumboVector *kids = gumbo_children_vector(fr->src_parent);
        if (!kids || fr->next_index >= kids->length) {
            g_queue_pop_head(&stack);
            g_free(fr);
            continue;
        }
        const GumboNode *child = (const GumboNode *)kids->data[fr->next_index++];
        nd_node *converted = gumbo_convert_self(child);
        if (!converted) continue;
        nd_node_append_child(fr->nd_parent, converted);
        gumbo_walk_push(&stack, child, converted);
    }
    return root_nd;
}

nd_node *
nd_html_parse_gumbo(const char *input, gssize len)
{
    if (!input) return NULL;
    gsize n = (len < 0) ? strlen(input) : (gsize)len;
    GumboOptions opts = kGumboDefaultOptions;
    GumboOutput *out = gumbo_parse_with_options(&opts, input, n);
    if (!out) return NULL;
    nd_node *root = gumbo_to_nd(out->document);
    gumbo_destroy_output(&opts, out);
    return root;
}

static nd_node *
find_child_named(nd_node *parent, const char *tag);

nd_node *
nd_html_parse_fragment_gumbo(const char *context_tag,
                             const char *input, gssize len)
{
    if (!input) return NULL;
    gsize n = (len < 0) ? strlen(input) : (gsize)len;
    GumboOptions opts = kGumboDefaultOptions;
    GumboTag ctx = GUMBO_TAG_BODY;
    if (context_tag && *context_tag) {
        GumboTag t = gumbo_tag_enum(context_tag);
        if (t != GUMBO_TAG_UNKNOWN) ctx = t;
    }
    opts.fragment_context = ctx;
    opts.fragment_namespace = GUMBO_NAMESPACE_HTML;
    GumboOutput *out = gumbo_parse_with_options(&opts, input, n);
    if (!out) return NULL;
    nd_node *doc = gumbo_to_nd(out->document);
    gumbo_destroy_output(&opts, out);
    if (!doc) return NULL;
    nd_node *html_el = find_child_named(doc, "html");
    nd_node *container = html_el ? html_el : doc;
    nd_node *body = find_child_named(container, "body");
    if (body) container = body;
    nd_node *frag = nd_node_new_document();
    nd_node *c = container->first_child;
    while (c) {
        nd_node *next = c->next_sibling;
        nd_node_remove(c);
        nd_node_append_child(frag, c);
        c = next;
    }
    nd_node_free(doc);
    return frag;
}

gboolean
nd_html_engine_lexbor_available(void)
{
#ifdef ND_HAVE_LEXBOR
    return TRUE;
#else
    return FALSE;
#endif
}

const char *
nd_html_engine_name(nd_html_engine engine)
{
    switch (engine) {
    case ND_HTML_ENGINE_LEXBOR: return "lexbor";
    case ND_HTML_ENGINE_GUMBO:  /* fallthrough */
    default:                    return "gumbo";
    }
}

static nd_html_engine g_default_engine = ND_HTML_ENGINE_LEXBOR;
static gboolean       g_default_engine_resolved;

static nd_html_engine
built_in_default_engine(void)
{
    return nd_html_engine_lexbor_available() ? ND_HTML_ENGINE_LEXBOR
                                             : ND_HTML_ENGINE_GUMBO;
}

static nd_html_engine
parse_engine_name(const char *name)
{
    if (!name || !*name) return built_in_default_engine();
    if (g_ascii_strcasecmp(name, "lexbor") == 0) return ND_HTML_ENGINE_LEXBOR;
    if (g_ascii_strcasecmp(name, "gumbo") == 0)  return ND_HTML_ENGINE_GUMBO;
    return built_in_default_engine();
}

static void
resolve_default_engine(void)
{
    if (g_default_engine_resolved) return;
    g_default_engine_resolved = TRUE;
    const char *env = g_getenv("ND_HTML_ENGINE");
    nd_html_engine requested = parse_engine_name(env);
    if (requested == ND_HTML_ENGINE_LEXBOR && !nd_html_engine_lexbor_available())
        requested = ND_HTML_ENGINE_GUMBO;
    g_default_engine = requested;
}

nd_html_engine
nd_html_engine_default(void)
{
    resolve_default_engine();
    return g_default_engine;
}

void
nd_html_engine_set_default(nd_html_engine engine)
{
    if (engine == ND_HTML_ENGINE_LEXBOR && !nd_html_engine_lexbor_available())
        engine = ND_HTML_ENGINE_GUMBO;
    g_default_engine = engine;
    g_default_engine_resolved = TRUE;
}

nd_node *
nd_html_parse_with(nd_html_engine engine, const char *input, gssize len)
{
#ifdef ND_HAVE_LEXBOR
    if (engine == ND_HTML_ENGINE_LEXBOR)
        return nd_html_parse_lexbor(input, len);
#else
    (void)engine;
#endif
    return nd_html_parse_gumbo(input, len);
}

nd_node *
nd_html_parse(const char *input, gssize len)
{
    return nd_html_parse_with(nd_html_engine_default(), input, len);
}

nd_node *
nd_html_parse_for_url(const char *url, const char *input, gssize len)
{
    nd_html_engine engine = nd_html_engine_default();
    if (url) nd_compat_html_engine_for_url(url, &engine);
    return nd_html_parse_with(engine, input, len);
}

nd_node *
nd_html_parse_for_page(const char *input, gssize len)
{
    return nd_html_parse(input, len);
}

static nd_node *
find_child_named(nd_node *parent, const char *tag)
{
    if (!parent) return NULL;
    for (nd_node *c = parent->first_child; c; c = c->next_sibling)
        if (c->kind == ND_NODE_ELEMENT && c->name
            && g_ascii_strcasecmp(c->name, tag) == 0)
            return c;
    return NULL;
}

nd_node *
nd_html_parse_fragment_with(nd_html_engine engine,
                            const char *context_tag,
                            const char *input, gssize len)
{
#ifdef ND_HAVE_LEXBOR
    if (engine == ND_HTML_ENGINE_LEXBOR)
        return nd_html_parse_fragment_lexbor(context_tag, input, len);
#else
    (void)engine;
#endif
    return nd_html_parse_fragment_gumbo(context_tag, input, len);
}

nd_node *
nd_html_parse_fragment_in(const char *context_tag,
                          const char *input, gssize len)
{
    return nd_html_parse_fragment_with(nd_html_engine_default(),
                                       context_tag, input, len);
}

nd_node *
nd_html_parse_fragment(const char *input, gssize len)
{
    return nd_html_parse_fragment_in(NULL, input, len);
}

char *
nd_html_decode_body(const char *body, gsize len)
{
    if (!body || len == 0) return g_strdup("");

    char *charset = NULL;
    uchardet_t det = uchardet_new();
    if (det) {
        gsize scan = len < 65536 ? len : 65536;
        if (uchardet_handle_data(det, body, scan) == 0) {
            uchardet_data_end(det);
            const char *name = uchardet_get_charset(det);
            if (name && *name) charset = g_strdup(name);
        }
        uchardet_delete(det);
    }
    if (!charset)
        charset = g_strdup(g_utf8_validate(body, (gssize)len, NULL)
                             ? "UTF-8" : "ISO-8859-1");

    GError *err = NULL;
    char *out = g_convert(body, (gssize)len, "UTF-8", charset,
                          NULL, NULL, &err);
    g_free(charset);
    g_clear_error(&err);
    return out ? out : g_strdup("(unable to decode response body)\n");
}
