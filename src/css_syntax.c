/* Northstar — CSS Syntax tokenizer and component-value parser. */

#include "css_syntax.h"

#include <string.h>

typedef struct ns_css_syntax_parser {
    const char *input;
    gsize length;
    gsize offset;
    gboolean valid;
} ns_css_syntax_parser;

static gboolean
css_syntax_whitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static gboolean
css_syntax_name_start(guchar c)
{
    return g_ascii_isalpha(c) || c == '_' || c >= 0x80;
}

static gboolean
css_syntax_name(guchar c)
{
    return css_syntax_name_start(c) || g_ascii_isdigit(c) || c == '-';
}

static gsize
css_syntax_escape_end(const char *input, gsize length, gsize offset)
{
    if (offset >= length || input[offset] != '\\') return offset;
    offset++;
    if (offset >= length) return offset;
    if (g_ascii_isxdigit(input[offset])) {
        guint digits = 0;
        while (offset < length && digits < 6 &&
               g_ascii_isxdigit(input[offset])) {
            offset++;
            digits++;
        }
        if (offset < length && css_syntax_whitespace(input[offset])) offset++;
        return offset;
    }
    if (input[offset] == '\n' || input[offset] == '\r' ||
        input[offset] == '\f')
        return offset - 1;
    return offset + 1;
}

static gsize
css_syntax_name_end(const char *input, gsize length, gsize offset)
{
    while (offset < length) {
        if (css_syntax_name((guchar)input[offset])) {
            offset++;
            continue;
        }
        if (input[offset] == '\\') {
            gsize next = css_syntax_escape_end(input, length, offset);
            if (next == offset) break;
            offset = next;
            continue;
        }
        break;
    }
    return offset;
}

static gboolean
css_syntax_valid_escape(const char *input, gsize length, gsize offset)
{
    return offset + 1 < length && input[offset] == '\\' &&
           input[offset + 1] != '\n' && input[offset + 1] != '\r' &&
           input[offset + 1] != '\f';
}

static gboolean
css_syntax_starts_ident(const char *input, gsize length, gsize offset)
{
    if (offset >= length) return FALSE;
    char a = input[offset];
    if (css_syntax_name_start((guchar)a)) return TRUE;
    if (a == '\\') return css_syntax_valid_escape(input, length, offset);
    if (a != '-') return FALSE;
    if (offset + 1 >= length) return FALSE;
    char b = input[offset + 1];
    return css_syntax_name_start((guchar)b) || b == '-' ||
           css_syntax_valid_escape(input, length, offset + 1);
}

static gboolean
css_syntax_starts_number(const char *input, gsize length, gsize offset)
{
    if (offset >= length) return FALSE;
    char a = input[offset];
    char b = offset + 1 < length ? input[offset + 1] : '\0';
    char c = offset + 2 < length ? input[offset + 2] : '\0';
    if (g_ascii_isdigit(a)) return TRUE;
    if (a == '.') return g_ascii_isdigit(b);
    if (a == '+' || a == '-')
        return g_ascii_isdigit(b) || (b == '.' && g_ascii_isdigit(c));
    return FALSE;
}

static gsize
css_syntax_number_end(const char *input, gsize length, gsize offset)
{
    if (offset < length && (input[offset] == '+' || input[offset] == '-'))
        offset++;
    while (offset < length && g_ascii_isdigit(input[offset])) offset++;
    if (offset + 1 < length && input[offset] == '.' &&
        g_ascii_isdigit(input[offset + 1])) {
        offset++;
        while (offset < length && g_ascii_isdigit(input[offset])) offset++;
    }
    if (offset < length && (input[offset] == 'e' || input[offset] == 'E')) {
        gsize exponent = offset + 1;
        if (exponent < length &&
            (input[exponent] == '+' || input[exponent] == '-'))
            exponent++;
        if (exponent < length && g_ascii_isdigit(input[exponent])) {
            offset = exponent + 1;
            while (offset < length && g_ascii_isdigit(input[offset])) offset++;
        }
    }
    return offset;
}

static ns_css_component *
css_component_new(ns_css_component_type type, gsize start)
{
    ns_css_component *component = g_new0(ns_css_component, 1);
    component->type = type;
    component->start = start;
    return component;
}

void
ns_css_component_free(ns_css_component *component)
{
    if (!component) return;
    g_free(component->value);
    if (component->children) g_ptr_array_free(component->children, TRUE);
    g_free(component);
}

static void
css_syntax_skip_comment(ns_css_syntax_parser *parser)
{
    parser->offset += 2;
    while (parser->offset + 1 < parser->length &&
           !(parser->input[parser->offset] == '*' &&
             parser->input[parser->offset + 1] == '/'))
        parser->offset++;
    if (parser->offset + 1 < parser->length)
        parser->offset += 2;
    else
        parser->offset = parser->length;
}

static GPtrArray *css_syntax_parse_list(ns_css_syntax_parser *parser,
                                        char closing);

static ns_css_component *
css_syntax_consume_string(ns_css_syntax_parser *parser)
{
    gsize start = parser->offset;
    char quote = parser->input[parser->offset++];
    GString *value = g_string_new(NULL);
    while (parser->offset < parser->length) {
        char c = parser->input[parser->offset++];
        if (c == quote) break;
        if (c == '\n' || c == '\r' || c == '\f') {
            parser->valid = FALSE;
            break;
        }
        if (c == '\\' && parser->offset < parser->length) {
            gsize escape = parser->offset - 1;
            gsize next = css_syntax_escape_end(parser->input, parser->length,
                                               escape);
            if (next == escape) {
                parser->valid = FALSE;
                break;
            }
            g_string_append_len(value, parser->input + escape,
                                (gssize)(next - escape));
            parser->offset = next;
        } else {
            g_string_append_c(value, c);
        }
    }
    ns_css_component *component =
        css_component_new(NS_CSS_COMPONENT_STRING, start);
    component->end = parser->offset;
    component->value = g_string_free(value, FALSE);
    component->delimiter = quote;
    return component;
}

static ns_css_component *
css_syntax_consume_numeric(ns_css_syntax_parser *parser)
{
    gsize start = parser->offset;
    gsize number_end = css_syntax_number_end(parser->input, parser->length,
                                             start);
    char *number = g_strndup(parser->input + start, number_end - start);
    ns_css_component_type type = NS_CSS_COMPONENT_NUMBER;
    gsize end = number_end;
    if (end < parser->length && parser->input[end] == '%') {
        type = NS_CSS_COMPONENT_PERCENTAGE;
        end++;
    } else if (css_syntax_starts_ident(parser->input, parser->length, end)) {
        type = NS_CSS_COMPONENT_DIMENSION;
        end = css_syntax_name_end(parser->input, parser->length, end);
    }
    ns_css_component *component = css_component_new(type, start);
    component->end = end;
    component->number = g_ascii_strtod(number, NULL);
    component->value = g_strndup(parser->input + number_end, end - number_end);
    parser->offset = end;
    g_free(number);
    return component;
}

static ns_css_component *
css_syntax_consume_ident(ns_css_syntax_parser *parser, gboolean at,
                         gboolean hash)
{
    gsize start = parser->offset;
    if (at || hash) parser->offset++;
    gsize name_start = parser->offset;
    parser->offset = css_syntax_name_end(parser->input, parser->length,
                                         parser->offset);
    ns_css_component_type type = at ? NS_CSS_COMPONENT_AT_KEYWORD
                                    : hash ? NS_CSS_COMPONENT_HASH
                                           : NS_CSS_COMPONENT_IDENT;
    if (!at && !hash && parser->offset < parser->length &&
        parser->input[parser->offset] == '(') {
        type = NS_CSS_COMPONENT_FUNCTION;
        parser->offset++;
    }
    ns_css_component *component = css_component_new(type, start);
    component->value = g_strndup(parser->input + name_start,
                                 parser->offset - name_start -
                                 (type == NS_CSS_COMPONENT_FUNCTION ? 1 : 0));
    if (type == NS_CSS_COMPONENT_FUNCTION) {
        component->children = css_syntax_parse_list(parser, ')');
        component->delimiter = ')';
    }
    component->end = parser->offset;
    return component;
}

static ns_css_component *
css_syntax_consume(ns_css_syntax_parser *parser)
{
    gsize start = parser->offset;
    char c = parser->input[parser->offset];
    if (css_syntax_whitespace(c)) {
        while (parser->offset < parser->length &&
               css_syntax_whitespace(parser->input[parser->offset]))
            parser->offset++;
        ns_css_component *component =
            css_component_new(NS_CSS_COMPONENT_WHITESPACE, start);
        component->end = parser->offset;
        return component;
    }
    if (c == '"' || c == '\'') return css_syntax_consume_string(parser);
    if (css_syntax_starts_number(parser->input, parser->length,
                                 parser->offset))
        return css_syntax_consume_numeric(parser);
    if (c == '@' &&
        css_syntax_starts_ident(parser->input, parser->length,
                                parser->offset + 1))
        return css_syntax_consume_ident(parser, TRUE, FALSE);
    if (c == '#' && parser->offset + 1 < parser->length &&
        (css_syntax_name((guchar)parser->input[parser->offset + 1]) ||
         css_syntax_valid_escape(parser->input, parser->length,
                                 parser->offset + 1)))
        return css_syntax_consume_ident(parser, FALSE, TRUE);
    if (css_syntax_starts_ident(parser->input, parser->length, parser->offset))
        return css_syntax_consume_ident(parser, FALSE, FALSE);
    parser->offset++;
    ns_css_component_type type = c == ':' ? NS_CSS_COMPONENT_COLON
        : c == ';' ? NS_CSS_COMPONENT_SEMICOLON
        : c == ',' ? NS_CSS_COMPONENT_COMMA : NS_CSS_COMPONENT_DELIM;
    ns_css_component *component = css_component_new(type, start);
    component->end = parser->offset;
    component->delimiter = c;
    if (c == '(' || c == '[' || c == '{') {
        component->type = NS_CSS_COMPONENT_BLOCK;
        component->delimiter = c == '(' ? ')' : c == '[' ? ']' : '}';
        component->children = css_syntax_parse_list(parser,
                                                     component->delimiter);
        component->end = parser->offset;
    }
    return component;
}

static GPtrArray *
css_syntax_parse_list(ns_css_syntax_parser *parser, char closing)
{
    GPtrArray *components =
        g_ptr_array_new_with_free_func((GDestroyNotify)ns_css_component_free);
    while (parser->offset < parser->length) {
        char c = parser->input[parser->offset];
        if (c == '/' && parser->offset + 1 < parser->length &&
            parser->input[parser->offset + 1] == '*') {
            css_syntax_skip_comment(parser);
            continue;
        }
        if (closing && c == closing) {
            parser->offset++;
            return components;
        }
        if (c == ')' || c == ']' || c == '}') {
            parser->valid = FALSE;
            parser->offset++;
            if (closing) return components;
            continue;
        }
        g_ptr_array_add(components, css_syntax_consume(parser));
    }
    return components;
}

GPtrArray *
ns_css_component_values_parse(const char *input, gssize len, gboolean *valid)
{
    ns_css_syntax_parser parser = {
        .input = input ? input : "",
        .length = input ? (len < 0 ? strlen(input) : (gsize)len) : 0,
        .valid = TRUE,
    };
    GPtrArray *components = css_syntax_parse_list(&parser, 0);
    if (valid) *valid = parser.valid;
    return components;
}

const char *
ns_css_syntax_scan(const char *input, const char *end,
                   const char *terminators, char *terminator)
{
    const char *p = input;
    char quote = 0;
    char stack[128];
    guint depth = 0;
    if (terminator) *terminator = 0;
    while (p < end) {
        char c = *p;
        if (quote) {
            if (c == '\\' && p + 1 < end) {
                p += 2;
                continue;
            }
            if (c == quote) quote = 0;
            p++;
            continue;
        }
        if (c == '/' && p + 1 < end && p[1] == '*') {
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++;
            p = p + 1 < end ? p + 2 : end;
            continue;
        }
        if (c == '\\' && p + 1 < end) {
            p += 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            p++;
            continue;
        }
        if (depth == 0 && strchr(terminators, c)) {
            if (terminator) *terminator = c;
            return p;
        }
        if (c == '(' || c == '[' || c == '{') {
            if (depth < G_N_ELEMENTS(stack))
                stack[depth++] = c == '(' ? ')' : c == '[' ? ']' : '}';
        } else if (depth > 0 && c == stack[depth - 1]) {
            depth--;
        }
        p++;
    }
    return p;
}

gboolean
ns_css_component_value_valid(const char *input)
{
    gboolean valid = FALSE;
    GPtrArray *components =
        ns_css_component_values_parse(input ? input : "", -1, &valid);
    g_ptr_array_free(components, TRUE);
    return valid;
}
