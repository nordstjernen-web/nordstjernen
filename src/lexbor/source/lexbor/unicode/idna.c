/*
 * Copyright (C) 2023 Alexander Borisov
 *
 * Author: Alexander Borisov <borisov@lexbor.com>
 */

#include "lexbor/unicode/idna.h"
#include "lexbor/unicode/unicode.h"
#include "lexbor/punycode/punycode.h"
#include "lexbor/encoding/encoding.h"


typedef struct {
    lxb_unicode_idna_cb_f   cb;
    void                    *context;
    lxb_unicode_idna_flag_t flags;
    lxb_unicode_idna_t      *idna;
    bool                    bidi_domain;
}
lxb_unicode_idna_ctx_t;

typedef struct {
    lxb_codepoint_t buffer[512];
    lxb_codepoint_t *data;
    size_t          length;
    size_t          capacity;
}
lxb_unicode_idna_cps_t;

typedef struct {
    lxb_char_t              buffer[4096];
    lxb_char_t              *p;
    lxb_char_t              *buf;
    const lxb_char_t        *end;
    lxb_unicode_idna_flag_t flags;
}
lxb_unicode_idna_ascii_ctx_t;


static lxb_status_t
lxb_unicode_idna_processing_body(lxb_unicode_idna_t *idna, const void *data,
                                 size_t len, lxb_unicode_idna_cb_f cb, void *ctx,
                                 lxb_unicode_idna_flag_t flags, bool is_cp);

static lxb_status_t
lxb_unicode_idna_norm_c_cb(const lxb_codepoint_t *cps, size_t len, void *ctx);

static lxb_status_t
lxb_unicode_idna_label_send(const lxb_codepoint_t *cps, size_t len,
                            lxb_unicode_idna_ctx_t *context);

static bool
lxb_unicode_idna_label_valid(lxb_unicode_idna_t *idna,
                             const lxb_codepoint_t *cps, size_t len,
                             lxb_unicode_idna_flag_t flags, bool bidi_domain);

static lxb_status_t
lxb_unicode_idna_to_ascii_cb(const lxb_codepoint_t *part, size_t len,
                             void *ctx, lxb_status_t status);

static lxb_status_t
lxb_unicode_idna_to_ascii_body(lxb_unicode_idna_t *idna, const void *data,
                               size_t length, lexbor_serialize_cb_f cb, void *ctx,
                               lxb_unicode_idna_flag_t flags, bool is_cp);

static lxb_status_t
lxb_unicode_idna_ascii_puny_cb(const lxb_char_t *data, size_t length, void *ctx,
                               bool unchanged);

static lxb_status_t
lxb_unicode_idna_to_unicode_cb(const lxb_codepoint_t *part, size_t len,
                               void *ctx, lxb_status_t status);

static lxb_status_t
lxb_unicode_idna_to_unicode_body(lxb_unicode_idna_t *idna, const void *data,
                                 size_t length, lexbor_serialize_cb_f cb,
                                 void *ctx, lxb_unicode_idna_flag_t flags,
                                 bool is_cp);

lxb_unicode_idna_t *
lxb_unicode_idna_create(void)
{
    return lexbor_malloc(sizeof(lxb_unicode_idna_t));
}

lxb_status_t
lxb_unicode_idna_init(lxb_unicode_idna_t *idna)
{
    lxb_status_t status;

    if (idna == NULL) {
        return LXB_STATUS_ERROR_OBJECT_IS_NULL;
    }

    status = lxb_unicode_normalizer_init(&idna->normalizer, LXB_UNICODE_NFC);
    if (status != LXB_STATUS_OK) {
        return status;
    }

    return lxb_unicode_normalizer_init(&idna->nfc_check, LXB_UNICODE_NFC);
}

void
lxb_unicode_idna_clean(lxb_unicode_idna_t *idna)
{
    lxb_unicode_normalizer_clean(&idna->normalizer);
    lxb_unicode_normalizer_clean(&idna->nfc_check);
}

lxb_unicode_idna_t *
lxb_unicode_idna_destroy(lxb_unicode_idna_t *idna, bool self_destroy)
{
    if (idna == NULL) {
        return NULL;
    }

    (void) lxb_unicode_normalizer_destroy(&idna->normalizer, false);
    (void) lxb_unicode_normalizer_destroy(&idna->nfc_check, false);

    if (self_destroy) {
        return lexbor_free(idna);
    }

    return idna;
}

lxb_codepoint_t *
lxb_unicode_idna_realloc(lxb_codepoint_t *buf, const lxb_codepoint_t *buffer,
                         lxb_codepoint_t **buf_p, lxb_codepoint_t **buf_end,
                         size_t len)
{
    size_t nlen;
    lxb_codepoint_t *tmp;

    nlen = ((*buf_end - buf) * 4) + len;
 
    if (buf == buffer) {
        tmp = lexbor_malloc(nlen * sizeof(lxb_codepoint_t));
        if (tmp == NULL) {
            return NULL;
        }
    }
    else {
        tmp = lexbor_realloc(buf, nlen * sizeof(lxb_codepoint_t));
        if (tmp == NULL) {
            return lexbor_free(buf);
        }
    }

    *buf_p = tmp + (*buf_p - buf);
    *buf_end = tmp + nlen;

    return tmp;
}

lxb_status_t
lxb_unicode_idna_processing(lxb_unicode_idna_t *idna, const lxb_char_t *data,
                            size_t length, lxb_unicode_idna_cb_f cb, void *ctx,
                            lxb_unicode_idna_flag_t flags)
{
    return lxb_unicode_idna_processing_body(idna, data, length, cb, ctx,
                                            flags, false);
}

lxb_status_t
lxb_unicode_idna_processing_cp(lxb_unicode_idna_t *idna,
                               const lxb_codepoint_t *cps, size_t length,
                               lxb_unicode_idna_cb_f cb, void *ctx,
                               lxb_unicode_idna_flag_t flags)
{
    return lxb_unicode_idna_processing_body(idna, cps, length, cb, ctx,
                                            flags, true);
}

static lxb_status_t
lxb_unicode_idna_processing_body(lxb_unicode_idna_t *idna, const void *data,
                                 size_t len, lxb_unicode_idna_cb_f cb, void *ctx,
                                 lxb_unicode_idna_flag_t flags, bool is_cp)
{
    bool need;
    size_t i, length;
    lxb_status_t status;
    lxb_codepoint_t cp, *buf, *buf_p, *buf_end;
    const lxb_char_t *end, *p;
    lxb_unicode_idna_type_t type;
    const lxb_unicode_idna_entry_t *udata;
    const lxb_codepoint_t *maps;
    lxb_unicode_idna_ctx_t context;
    lxb_codepoint_t buffer[4096];

    buf = buffer;
    buf_p = buffer;
    buf_end = buffer + (sizeof(buffer) / sizeof(lxb_codepoint_t));

    p = data;
    len *= (is_cp) ? sizeof(lxb_codepoint_t) : 1;
    end = (const lxb_char_t *) data + len;

    while (p < end) {
        if (is_cp) {
            cp = *((const lxb_codepoint_t *) p);
            p = (const lxb_char_t *) ((const lxb_codepoint_t *) p + 1);
        }
        else {
            cp = lxb_encoding_decode_valid_utf_8_single(&p, end);
            if (cp > LXB_ENCODING_DECODE_MAX_CODEPOINT) {
                status = LXB_STATUS_ERROR_UNEXPECTED_DATA;
                goto done;
            }
        }

        type = lxb_unicode_idna_type(cp);

    again:

        switch (type) {
            case LXB_UNICODE_IDNA_IGNORED:
                break;

            case LXB_UNICODE_IDNA_MAPPED:
                udata = lxb_unicode_idna_entry_by_cp(cp);
                maps = lxb_unicode_idna_map(udata, &length);

                if (buf_p + length > buf_end) {
                    buf = lxb_unicode_idna_realloc(buf, buffer, &buf_p,
                                                   &buf_end, length);
                    if (buf == NULL) {
                        return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
                    }
                }

                for (i = 0; i < length; i++) {
                    *buf_p++ = maps[i];
                }

                break;

            case LXB_UNICODE_IDNA_DEVIATION:
                if ((flags & LXB_UNICODE_IDNA_FLAG_TRANSITIONAL_PROCESSING)) {
                    type = LXB_UNICODE_IDNA_MAPPED;
                    goto again;
                }

                /* Fall through. */

            case LXB_UNICODE_IDNA_DISALLOWED:
                /* Fall through. */

            case LXB_UNICODE_IDNA_VALID:
            default:
                if (buf_p >= buf_end) {
                    buf = lxb_unicode_idna_realloc(buf, buffer, &buf_p,
                                                   &buf_end, 1);
                    if (buf == NULL) {
                        return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
                    }
                }

                *buf_p++ = cp;
                break;
        }
    }

    context.cb = cb;
    context.context = ctx;
    context.flags = flags;
    context.idna = idna;
    context.bidi_domain = false;

    need = lxb_unicode_quick_check_cp(&idna->normalizer, buf, buf_p - buf,
                                      true);
    if (need) {
        lxb_unicode_flush_count_set(&idna->normalizer, UINT32_MAX);

        status = lxb_unicode_normalize_cp(&idna->normalizer, buf, buf_p - buf,
                                          lxb_unicode_idna_norm_c_cb,
                                          &context, true);
    }
    else {
        status = lxb_unicode_idna_norm_c_cb(buf, buf_p - buf, &context);
    }

done:

    if (buf != buffer) {
        (void) lexbor_free(buf);
    }

    return status;
}

static bool
lxb_unicode_idna_label_is_puny(const lxb_codepoint_t *cps, size_t len)
{
    return len >= 4
        && (cps[0] == 0x0078 || cps[0] == 0x0058)
        && (cps[1] == 0x006E || cps[1] == 0x004E)
        && cps[2] == 0x002D && cps[3] == 0x002D;
}

static void
lxb_unicode_idna_cps_init(lxb_unicode_idna_cps_t *col)
{
    col->data = col->buffer;
    col->length = 0;
    col->capacity = sizeof(col->buffer) / sizeof(lxb_codepoint_t);
}

static void
lxb_unicode_idna_cps_release(lxb_unicode_idna_cps_t *col)
{
    if (col->data != col->buffer) {
        (void) lexbor_free(col->data);
    }
}

static lxb_status_t
lxb_unicode_idna_cps_cb(const lxb_codepoint_t *cps, size_t len, void *ctx)
{
    size_t ncap;
    lxb_codepoint_t *tmp;
    lxb_unicode_idna_cps_t *col = ctx;

    if (col->length + len > col->capacity) {
        ncap = (col->capacity * 4) + len;

        if (col->data == col->buffer) {
            tmp = lexbor_malloc(ncap * sizeof(lxb_codepoint_t));
            if (tmp != NULL) {
                memcpy(tmp, col->buffer,
                       col->length * sizeof(lxb_codepoint_t));
            }
        }
        else {
            tmp = lexbor_realloc(col->data, ncap * sizeof(lxb_codepoint_t));
        }

        if (tmp == NULL) {
            return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
        }

        col->data = tmp;
        col->capacity = ncap;
    }

    memcpy(col->data + col->length, cps, len * sizeof(lxb_codepoint_t));
    col->length += len;

    return LXB_STATUS_OK;
}

static bool
lxb_unicode_idna_has_rtl(const lxb_codepoint_t *cps, size_t len)
{
    size_t i;
    lxb_unicode_idna_bidi_t bd;

    for (i = 0; i < len; i++) {
        bd = lxb_unicode_idna_props_bidi(lxb_unicode_idna_validity_props(cps[i]));

        if (bd == LXB_UNICODE_IDNA_BIDI_R || bd == LXB_UNICODE_IDNA_BIDI_AL
            || bd == LXB_UNICODE_IDNA_BIDI_AN)
        {
            return true;
        }
    }

    return false;
}

static bool
lxb_unicode_idna_label_nfc(lxb_unicode_idna_t *idna,
                           const lxb_codepoint_t *cps, size_t len)
{
    bool same;
    lxb_status_t status;
    lxb_unicode_idna_cps_t col;

    if (idna == NULL || len == 0) {
        return true;
    }

    if (!lxb_unicode_quick_check_cp(&idna->nfc_check, cps, len, true)) {
        return true;
    }

    lxb_unicode_idna_cps_init(&col);
    lxb_unicode_flush_count_set(&idna->nfc_check, UINT32_MAX);

    status = lxb_unicode_normalize_cp(&idna->nfc_check, cps, len,
                                      lxb_unicode_idna_cps_cb, &col, true);

    same = status == LXB_STATUS_OK && col.length == len
           && memcmp(col.data, cps, len * sizeof(lxb_codepoint_t)) == 0;

    lxb_unicode_idna_cps_release(&col);
    lxb_unicode_normalizer_clean(&idna->nfc_check);

    return same;
}

static lxb_status_t
lxb_unicode_idna_label_send(const lxb_codepoint_t *cps, size_t len,
                            lxb_unicode_idna_ctx_t *context)
{
    size_t i;
    lxb_status_t status;
    lxb_unicode_idna_cps_t col;
    const lxb_codepoint_t *final;
    size_t final_len;

    lxb_unicode_idna_cps_init(&col);

    if (lxb_unicode_idna_label_is_puny(cps, len)) {
        bool puny_ok = true;

        for (i = 4; i < len; i++) {
            if (cps[i] >= 0x80) {
                puny_ok = false;
                break;
            }
        }

        if (puny_ok) {
            status = lxb_punycode_decode_cp(cps + 4, len - 4,
                                            lxb_unicode_idna_cps_cb, &col);
            if (status != LXB_STATUS_OK || col.length == 0) {
                puny_ok = false;
            }
        }

        if (puny_ok) {
            final = col.data;
            final_len = col.length;

            if (!lxb_unicode_idna_label_nfc(context->idna, final, final_len)) {
                puny_ok = false;
            }
        }

        if (puny_ok &&
            !lxb_unicode_idna_label_valid(context->idna, final, final_len,
                                          context->flags,
                                          context->bidi_domain))
        {
            puny_ok = false;
        }

        if (!puny_ok) {
            lxb_unicode_idna_cps_release(&col);
            return context->cb(cps, len, context->context, LXB_STATUS_OK);
        }
    }
    else {
        final = cps;
        final_len = len;

        if (!lxb_unicode_idna_label_valid(context->idna, final, final_len,
                                          context->flags,
                                          context->bidi_domain))
        {
            lxb_unicode_idna_cps_release(&col);
            return LXB_STATUS_ERROR_UNEXPECTED_RESULT;
        }
    }

    status = context->cb(final, final_len, context->context, LXB_STATUS_OK);

    lxb_unicode_idna_cps_release(&col);

    return status;
}

static lxb_status_t
lxb_unicode_idna_bidi_scan(const lxb_codepoint_t *cps, size_t len,
                           lxb_unicode_idna_ctx_t *context)
{
    lxb_status_t status;
    lxb_unicode_idna_cps_t col;

    if (lxb_unicode_idna_label_is_puny(cps, len)) {
        lxb_unicode_idna_cps_init(&col);

        status = lxb_punycode_decode_cp(cps + 4, len - 4,
                                        lxb_unicode_idna_cps_cb, &col);
        if (status == LXB_STATUS_OK
            && lxb_unicode_idna_has_rtl(col.data, col.length))
        {
            context->bidi_domain = true;
        }

        lxb_unicode_idna_cps_release(&col);
    }
    else if (lxb_unicode_idna_has_rtl(cps, len)) {
        context->bidi_domain = true;
    }

    return LXB_STATUS_OK;
}

typedef lxb_status_t
(*lxb_unicode_idna_label_f)(const lxb_codepoint_t *cps, size_t len,
                            lxb_unicode_idna_ctx_t *context);

static lxb_status_t
lxb_unicode_idna_each_label(const lxb_codepoint_t *cps, size_t len,
                            lxb_unicode_idna_ctx_t *context,
                            lxb_unicode_idna_label_f fn)
{
    lxb_status_t status;
    const lxb_codepoint_t *p, *end;

    p = cps;
    end = cps + len;

    while (p < end) {
        if (*p == 0x002E) {
            status = fn(cps, p - cps, context);
            if (status != LXB_STATUS_OK) {
                return status;
            }

            cps = p + 1;
        }

        p += 1;
    }

    if (p > cps || (len >= 1 && p[-1] == '.')) {
        return fn(cps, p - cps, context);
    }

    return LXB_STATUS_OK;
}

static lxb_status_t
lxb_unicode_idna_norm_c_cb(const lxb_codepoint_t *cps, size_t len, void *ctx)
{
    lxb_status_t status;
    lxb_unicode_idna_ctx_t *context = ctx;

    if (context->flags & LXB_UNICODE_IDNA_FLAG_CHECK_BIDI) {
        status = lxb_unicode_idna_each_label(cps, len, context,
                                             lxb_unicode_idna_bidi_scan);
        if (status != LXB_STATUS_OK) {
            return status;
        }
    }

    return lxb_unicode_idna_each_label(cps, len, context,
                                       lxb_unicode_idna_label_send);
}

lxb_status_t
lxb_unicode_idna_to_ascii(lxb_unicode_idna_t *idna, const lxb_char_t *data,
                          size_t length, lexbor_serialize_cb_f cb, void *ctx,
                          lxb_unicode_idna_flag_t flags)
{
    return lxb_unicode_idna_to_ascii_body(idna, data, length, cb, ctx,
                                          flags, false);
}

lxb_status_t
lxb_unicode_idna_to_ascii_cp(lxb_unicode_idna_t *idna, const lxb_codepoint_t *cps,
                             size_t length, lexbor_serialize_cb_f cb, void *ctx,
                             lxb_unicode_idna_flag_t flags)
{
    return lxb_unicode_idna_to_ascii_body(idna, cps, length, cb, ctx,
                                          flags, true);
}

static lxb_status_t
lxb_unicode_idna_to_ascii_body(lxb_unicode_idna_t *idna, const void *data,
                               size_t length, lexbor_serialize_cb_f cb, void *ctx,
                               lxb_unicode_idna_flag_t flags, bool is_cp)
{
    size_t len;
    lxb_status_t status;
    lxb_unicode_idna_ascii_ctx_t context;

    context.p = context.buffer;
    context.buf = context.buffer;
    context.end = context.buf + sizeof(context.buffer);
    context.flags = flags;

    if (!is_cp) {
        status = lxb_unicode_idna_processing(idna, data, length,
                                             lxb_unicode_idna_to_ascii_cb,
                                             &context, flags);
    }
    else {
        status = lxb_unicode_idna_processing_cp(idna, data, length,
                                                lxb_unicode_idna_to_ascii_cb,
                                                &context, flags);
    }

    if (status != LXB_STATUS_OK) {
        goto done;
    }

    /* Remove last U+002E ( . ) FULL STOP. */

    if (context.p > context.buf) {
        context.p -= 1;
    }

    len = context.p - context.buf;

    status = cb(context.buf, len, ctx);

done:

    if (context.buf != context.buffer) {
        (void) lexbor_free(context.buf);
    }

    return status;
}

static lxb_status_t
lxb_unicode_idna_to_ascii_cb(const lxb_codepoint_t *part, size_t len,
                             void *ctx, lxb_status_t status)
{
    if (status != LXB_STATUS_OK) {
        return status;
    }

    return lxb_punycode_encode_cp(part, len, lxb_unicode_idna_ascii_puny_cb,
                                  ctx);
}

static lxb_status_t
lxb_unicode_idna_ascii_puny_cb(const lxb_char_t *data, size_t length, void *ctx,
                               bool unchanged)
{
    size_t nlen;
    lxb_char_t *tmp;
    lxb_unicode_idna_ascii_ctx_t *asc = ctx;

    static const lexbor_str_t prefix = lexbor_str("xn--");

    if (asc->p + length + 6 > asc->end) {
        nlen = ((asc->end - asc->buf) * 4) + length + 6;

        if (asc->buf == asc->buffer) {
            tmp = lexbor_malloc(nlen);
        }
        else {
            tmp = lexbor_realloc(asc->buf, nlen);
        }

        if (tmp == NULL) {
            return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
        }

        asc->p = tmp + (asc->p - asc->buf);
        asc->buf = tmp;
        asc->end = tmp + nlen;
    }

    if (!unchanged) {
        memcpy(asc->p, prefix.data, prefix.length);
        asc->p += 4;
    }

    memcpy(asc->p, data, length);

    asc->p += length;
    *asc->p++ = '.';
    *asc->p = 0x00;

    return LXB_STATUS_OK;
}

static bool
lxb_unicode_idna_contextj_valid(const lxb_codepoint_t *cps, size_t len,
                                size_t i)
{
    size_t j, k;
    lxb_unicode_idna_join_t jt;

    if (i == 0) {
        return false;
    }

    if (lxb_unicode_idna_validity_props(cps[i - 1])
        & LXB_UNICODE_IDNA_PROP_VIRAMA)
    {
        return true;
    }

    if (cps[i] == 0x200D) {
        return false;
    }

    j = i;

    while (j > 0) {
        jt = lxb_unicode_idna_props_join(
                 lxb_unicode_idna_validity_props(cps[j - 1]));

        if (jt != LXB_UNICODE_IDNA_JOIN_T) {
            break;
        }

        j -= 1;
    }

    if (j == 0) {
        return false;
    }

    jt = lxb_unicode_idna_props_join(
             lxb_unicode_idna_validity_props(cps[j - 1]));

    if (jt != LXB_UNICODE_IDNA_JOIN_L && jt != LXB_UNICODE_IDNA_JOIN_D) {
        return false;
    }

    k = i + 1;

    while (k < len) {
        jt = lxb_unicode_idna_props_join(
                 lxb_unicode_idna_validity_props(cps[k]));

        if (jt != LXB_UNICODE_IDNA_JOIN_T) {
            break;
        }

        k += 1;
    }

    if (k >= len) {
        return false;
    }

    jt = lxb_unicode_idna_props_join(lxb_unicode_idna_validity_props(cps[k]));

    return jt == LXB_UNICODE_IDNA_JOIN_R || jt == LXB_UNICODE_IDNA_JOIN_D;
}

static bool
lxb_unicode_idna_bidi_valid(const lxb_codepoint_t *cps, size_t len)
{
    bool rtl, has_en, has_an;
    size_t i;
    lxb_unicode_idna_bidi_t bd, first, last;

    first = lxb_unicode_idna_props_bidi(
                lxb_unicode_idna_validity_props(cps[0]));

    if (first == LXB_UNICODE_IDNA_BIDI_L) {
        rtl = false;
    }
    else if (first == LXB_UNICODE_IDNA_BIDI_R
             || first == LXB_UNICODE_IDNA_BIDI_AL)
    {
        rtl = true;
    }
    else {
        return false;
    }

    has_en = false;
    has_an = false;
    last = LXB_UNICODE_IDNA_BIDI_OTHER;

    for (i = 0; i < len; i++) {
        bd = lxb_unicode_idna_props_bidi(
                 lxb_unicode_idna_validity_props(cps[i]));

        switch (bd) {
            case LXB_UNICODE_IDNA_BIDI_EN:
                has_en = true;
                break;

            case LXB_UNICODE_IDNA_BIDI_ES:
            case LXB_UNICODE_IDNA_BIDI_CS:
            case LXB_UNICODE_IDNA_BIDI_ET:
            case LXB_UNICODE_IDNA_BIDI_ON:
            case LXB_UNICODE_IDNA_BIDI_BN:
            case LXB_UNICODE_IDNA_BIDI_NSM:
                break;

            case LXB_UNICODE_IDNA_BIDI_L:
                if (rtl) {
                    return false;
                }

                break;

            case LXB_UNICODE_IDNA_BIDI_R:
            case LXB_UNICODE_IDNA_BIDI_AL:
                if (!rtl) {
                    return false;
                }

                break;

            case LXB_UNICODE_IDNA_BIDI_AN:
                if (!rtl) {
                    return false;
                }

                has_an = true;
                break;

            default:
                return false;
        }
    }

    if (rtl && has_en && has_an) {
        return false;
    }

    i = len;

    while (i > 0) {
        last = lxb_unicode_idna_props_bidi(
                   lxb_unicode_idna_validity_props(cps[i - 1]));

        if (last != LXB_UNICODE_IDNA_BIDI_NSM) {
            break;
        }

        i -= 1;
    }

    if (i == 0) {
        return false;
    }

    if (rtl) {
        return last == LXB_UNICODE_IDNA_BIDI_R
            || last == LXB_UNICODE_IDNA_BIDI_AL
            || last == LXB_UNICODE_IDNA_BIDI_EN
            || last == LXB_UNICODE_IDNA_BIDI_AN;
    }

    return last == LXB_UNICODE_IDNA_BIDI_L || last == LXB_UNICODE_IDNA_BIDI_EN;
}

static bool
lxb_unicode_idna_label_valid(lxb_unicode_idna_t *idna,
                             const lxb_codepoint_t *cps, size_t len,
                             lxb_unicode_idna_flag_t flags, bool bidi_domain)
{
    size_t i;
    lxb_unicode_idna_type_t type;

    (void) idna;

    if (len == 0) {
        return true;
    }

    if (flags & LXB_UNICODE_IDNA_FLAG_CHECK_HYPHENS) {
        if (len >= 4 && cps[2] == 0x002D && cps[3] == 0x002D) {
            return false;
        }

        if (cps[0] == 0x002D || cps[len - 1] == 0x002D) {
            return false;
        }
    }
    else if (lxb_unicode_idna_label_is_puny(cps, len)) {
        return false;
    }

    if (lxb_unicode_idna_validity_props(cps[0]) & LXB_UNICODE_IDNA_PROP_MARK) {
        return false;
    }

    for (i = 0; i < len; i++) {
        if (cps[i] == 0x002E) {
            return false;
        }

        type = lxb_unicode_idna_type(cps[i]);

        switch (type) {
            case LXB_UNICODE_IDNA_VALID:
                break;

            case LXB_UNICODE_IDNA_DEVIATION:
                if (!(flags & LXB_UNICODE_IDNA_FLAG_TRANSITIONAL_PROCESSING)) {
                    break;
                }

                /* Fall through. */

            case LXB_UNICODE_IDNA_DISALLOWED:
            case LXB_UNICODE_IDNA_IGNORED:
            case LXB_UNICODE_IDNA_MAPPED:
            default:
                return false;
        }
    }

    if (flags & LXB_UNICODE_IDNA_FLAG_CHECK_JOINERS) {
        for (i = 0; i < len; i++) {
            if ((cps[i] == 0x200C || cps[i] == 0x200D)
                && !lxb_unicode_idna_contextj_valid(cps, len, i))
            {
                return false;
            }
        }
    }

    if ((flags & LXB_UNICODE_IDNA_FLAG_CHECK_BIDI) && bidi_domain) {
        if (!lxb_unicode_idna_bidi_valid(cps, len)) {
            return false;
        }
    }

    return true;
}

bool
lxb_unicode_idna_validity_criteria(const lxb_char_t *data, size_t length,
                                   lxb_unicode_idna_flag_t flags)
{
    bool valid;
    lxb_codepoint_t cp;
    const lxb_char_t *p, *end;
    lxb_unicode_idna_cps_t col;

    lxb_unicode_idna_cps_init(&col);

    p = data;
    end = data + length;

    while (p < end) {
        cp = lxb_encoding_decode_valid_utf_8_single(&p, end);
        if (cp == LXB_ENCODING_DECODE_ERROR) {
            lxb_unicode_idna_cps_release(&col);
            return false;
        }

        if (lxb_unicode_idna_cps_cb(&cp, 1, &col) != LXB_STATUS_OK) {
            lxb_unicode_idna_cps_release(&col);
            return false;
        }
    }

    valid = lxb_unicode_idna_validity_criteria_cp(col.data, col.length, flags);

    lxb_unicode_idna_cps_release(&col);

    return valid;
}

bool
lxb_unicode_idna_validity_criteria_cp(const lxb_codepoint_t *data, size_t length,
                                      lxb_unicode_idna_flag_t flags)
{
    return lxb_unicode_idna_label_valid(NULL, data, length, flags,
                                        lxb_unicode_idna_has_rtl(data, length));
}

lxb_status_t
lxb_unicode_idna_to_unicode(lxb_unicode_idna_t *idna, const lxb_char_t *data,
                            size_t length, lexbor_serialize_cb_f cb,
                            void *ctx, lxb_unicode_idna_flag_t flags)
{
    return lxb_unicode_idna_to_unicode_body(idna, data, length, cb, ctx,
                                            flags, false);
}

lxb_status_t
lxb_unicode_idna_to_unicode_cp(lxb_unicode_idna_t *idna,
                               const lxb_codepoint_t *cps,
                               size_t length, lexbor_serialize_cb_f cb,
                               void *ctx, lxb_unicode_idna_flag_t flags)
{
    return lxb_unicode_idna_to_unicode_body(idna, cps, length, cb, ctx,
                                            flags, true);
}

static lxb_status_t
lxb_unicode_idna_to_unicode_body(lxb_unicode_idna_t *idna, const void *data,
                                 size_t length, lexbor_serialize_cb_f cb,
                                 void *ctx, lxb_unicode_idna_flag_t flags,
                                 bool is_cp)
{
    size_t len;
    lxb_status_t status;
    lxb_unicode_idna_ascii_ctx_t context;

    context.p = context.buffer;
    context.buf = context.buffer;
    context.end = context.buf + sizeof(context.buffer);
    context.flags = flags;

    if (!is_cp) {
        status = lxb_unicode_idna_processing(idna, data, length,
                                             lxb_unicode_idna_to_unicode_cb,
                                             &context, flags);
    }
    else {
        status = lxb_unicode_idna_processing_cp(idna, data, length,
                                                lxb_unicode_idna_to_unicode_cb,
                                                &context, flags);
    }

    if (status != LXB_STATUS_OK) {
        goto done;
    }

    /* Remove last U+002E ( . ) FULL STOP. */

    if (context.p > context.buf) {
        context.p -= 1;
    }

    len = context.p - context.buf;

    status = cb(context.buf, len, ctx);

done:

    if (context.buf != context.buffer) {
        (void) lexbor_free(context.buf);
    }

    return status;
}


static lxb_status_t
lxb_unicode_idna_to_unicode_cb(const lxb_codepoint_t *part, size_t len,
                               void *ctx, lxb_status_t status)
{
    int8_t res;
    size_t length, nlen;
    lxb_char_t *tmp;
    const lxb_codepoint_t *p, *end;
    lxb_unicode_idna_ascii_ctx_t *asc = ctx;

    if (status != LXB_STATUS_OK) {
        return status;
    }

    p = part;
    end = part + len;

    length = 0;

    while (p < end) {
        res = lxb_encoding_encode_utf_8_length(*p++);
        if (res == 0) {
            return LXB_STATUS_ERROR_UNEXPECTED_DATA;
        }

        length += res;
    }

    if (asc->p + length + 2 > asc->end) {
        nlen = ((asc->end - asc->buf) * 4) + length + 2;

        if (asc->buf == asc->buffer) {
            tmp = lexbor_malloc(nlen);
        }
        else {
            tmp = lexbor_realloc(asc->buf, nlen);
        }

        if (tmp == NULL) {
            return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
        }

        asc->p = tmp + (asc->p - asc->buf);
        asc->buf = tmp;
        asc->end = tmp + nlen;
    }

    p = part;

    while (p < end) {
        (void) lxb_encoding_encode_utf_8_single(NULL, &asc->p, asc->end, *p++);
    }

    *asc->p++ = '.';
    *asc->p = 0x00;

    return LXB_STATUS_OK;
}
