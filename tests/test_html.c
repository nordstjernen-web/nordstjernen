/*
 * Nordstjernen — tests for the HTML parser + DOM.
 *
 * Linked against the source files directly (kept simple while the
 * tree is small; once we have a libnordstjernen static lib in meson,
 * tests will pick it up automatically).
 */

#include <glib.h>
#include <string.h>

#include "../src/dom.h"
#include "../src/html.h"

static void
test_dom_basic(void)
{
    nd_node *doc = nd_node_new_document();
    nd_node *html = nd_node_new_element(g_strdup("html"));
    nd_node *body = nd_node_new_element(g_strdup("body"));
    nd_node_append_child(doc, html);
    nd_node_append_child(html, body);

    nd_element_set_attr(body, "class", "main");
    nd_element_set_attr(body, "id", "x");
    nd_element_set_attr(body, "class", "main2"); /* overwrite */

    g_assert_cmpstr(nd_element_get_attr(body, "class"), ==, "main2");
    g_assert_cmpstr(nd_element_get_attr(body, "id"), ==, "x");
    g_assert_null(nd_element_get_attr(body, "missing"));

    nd_node_free(doc);
}

static void
test_html_minimal(void)
{
    const char *src = "<!doctype html><html><body><p>Hello</p></body></html>";
    nd_node *doc = nd_html_parse(src, -1);
    GString *dump = nd_node_dump(doc);
    g_assert_nonnull(strstr(dump->str, "#document"));
    g_assert_nonnull(strstr(dump->str, "<!DOCTYPE html>"));
    g_assert_nonnull(strstr(dump->str, "<html>"));
    g_assert_nonnull(strstr(dump->str, "<body>"));
    g_assert_nonnull(strstr(dump->str, "<p>"));
    g_assert_nonnull(strstr(dump->str, "\"Hello\""));
    g_string_free(dump, TRUE);
    nd_node_free(doc);
}

static void
test_html_attributes(void)
{
    const char *src =
        "<a href=\"https://example.com\" title='hi' data-x=42>link</a>";
    nd_node *doc = nd_html_parse(src, -1);

    /* find the <a> */
    nd_node *a = NULL;
    for (nd_node *c = doc->first_child; c; c = c->next_sibling) {
        if (c->kind == ND_NODE_ELEMENT && strcmp(c->name, "a") == 0) {
            a = c; break;
        }
    }
    g_assert_nonnull(a);
    g_assert_cmpstr(nd_element_get_attr(a, "href"), ==, "https://example.com");
    g_assert_cmpstr(nd_element_get_attr(a, "title"), ==, "hi");
    g_assert_cmpstr(nd_element_get_attr(a, "data-x"), ==, "42");

    nd_node_free(doc);
}

static void
test_html_void_and_self_closing(void)
{
    const char *src = "<div><br><img src=\"x\"/>after</div>";
    nd_node *doc = nd_html_parse(src, -1);
    GString *dump = nd_node_dump(doc);
    /* <br> should be a sibling of "after", not a parent of it. */
    g_assert_nonnull(strstr(dump->str, "<br>"));
    g_assert_nonnull(strstr(dump->str, "<img src=\"x\">"));
    g_assert_nonnull(strstr(dump->str, "\"after\""));

    /* Verify <br> has no children. */
    nd_node *div = doc->first_child;
    while (div && (div->kind != ND_NODE_ELEMENT || strcmp(div->name, "div") != 0))
        div = div->next_sibling;
    g_assert_nonnull(div);
    nd_node *br = div->first_child;
    while (br && (br->kind != ND_NODE_ELEMENT || strcmp(br->name, "br") != 0))
        br = br->next_sibling;
    g_assert_nonnull(br);
    g_assert_null(br->first_child);

    g_string_free(dump, TRUE);
    nd_node_free(doc);
}

static void
test_html_rawtext_script(void)
{
    const char *src =
        "<div><script>var x = 1 < 2 && 3 > 0; foo();</script>after</div>";
    nd_node *doc = nd_html_parse(src, -1);

    /* Find <script> and check it has a single text child containing the
     * raw "<" / "&" without being parsed as HTML. */
    nd_node *div = doc->first_child;
    while (div && (div->kind != ND_NODE_ELEMENT || strcmp(div->name, "div") != 0))
        div = div->next_sibling;
    g_assert_nonnull(div);
    nd_node *s = div->first_child;
    while (s && (s->kind != ND_NODE_ELEMENT || strcmp(s->name, "script") != 0))
        s = s->next_sibling;
    g_assert_nonnull(s);
    g_assert_nonnull(s->first_child);
    g_assert_cmpint(s->first_child->kind, ==, ND_NODE_TEXT);
    g_assert_nonnull(strstr(s->first_child->text, "1 < 2 && 3 > 0"));

    /* "after" should be in <div>, not in <script>. */
    GString *dump = nd_node_dump(doc);
    const char *script_pos = strstr(dump->str, "<script>");
    const char *after_pos = strstr(dump->str, "\"after\"");
    g_assert_nonnull(script_pos);
    g_assert_nonnull(after_pos);
    g_assert_true(after_pos > script_pos);

    g_string_free(dump, TRUE);
    nd_node_free(doc);
}

static void
test_html_entities(void)
{
    const char *src = "<p>R&amp;D &#x2014; &copy; &lt;ok&gt;</p>";
    nd_node *doc = nd_html_parse(src, -1);
    GString *dump = nd_node_dump(doc);
    /* Look for the decoded forms (in dump-escaped form). */
    g_assert_nonnull(strstr(dump->str, "R&D"));
    g_assert_nonnull(strstr(dump->str, "—"));
    g_assert_nonnull(strstr(dump->str, "©"));
    g_assert_nonnull(strstr(dump->str, "<ok>"));
    g_string_free(dump, TRUE);
    nd_node_free(doc);
}

static void
test_html_implicit_p_close(void)
{
    /* <p>one<p>two</p> — second <p> should close the first. */
    const char *src = "<body><p>one<p>two</p></body>";
    nd_node *doc = nd_html_parse(src, -1);
    nd_node *body = doc->first_child;
    while (body && (body->kind != ND_NODE_ELEMENT || strcmp(body->name, "body") != 0))
        body = body->next_sibling;
    g_assert_nonnull(body);
    int p_count = 0;
    for (nd_node *c = body->first_child; c; c = c->next_sibling) {
        if (c->kind == ND_NODE_ELEMENT && strcmp(c->name, "p") == 0) p_count++;
    }
    g_assert_cmpint(p_count, ==, 2);
    nd_node_free(doc);
}

static void
test_html_unmatched_end(void)
{
    /* Stray </div> with no opener should be ignored, not crash. */
    const char *src = "<p>hi</div></p>";
    nd_node *doc = nd_html_parse(src, -1);
    g_assert_nonnull(doc);
    nd_node_free(doc);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/dom/basic",            test_dom_basic);
    g_test_add_func("/html/minimal",         test_html_minimal);
    g_test_add_func("/html/attributes",      test_html_attributes);
    g_test_add_func("/html/void-selfclose",  test_html_void_and_self_closing);
    g_test_add_func("/html/rawtext-script",  test_html_rawtext_script);
    g_test_add_func("/html/entities",        test_html_entities);
    g_test_add_func("/html/implicit-p-close",test_html_implicit_p_close);
    g_test_add_func("/html/unmatched-end",   test_html_unmatched_end);
    return g_test_run();
}
