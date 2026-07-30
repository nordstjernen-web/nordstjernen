/* Northstar — offline Web Audio graph rendering (QuickJS).
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "js_internal.h"

#include <math.h>
#include <string.h>

#include <glib.h>

#define NS_WA_MAX_DEPTH 32

static void ns_wa_render(JSContext *ctx, JSValueConst node, uint32_t frames,
                         double rate, float *out, int depth);

static double
ns_wa_num(JSContext *ctx, JSValueConst obj, const char *name, double dflt)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    double d = dflt;
    if (!JS_IsUndefined(v) && !JS_IsNull(v) && JS_ToFloat64(ctx, &d, v) < 0)
        d = dflt;
    JS_FreeValue(ctx, v);
    return d;
}

static double
ns_wa_param(JSContext *ctx, JSValueConst node, const char *name, double dflt)
{
    JSValue p = JS_GetPropertyStr(ctx, node, name);
    double d = dflt;
    if (JS_IsObject(p)) d = ns_wa_num(ctx, p, "value", dflt);
    JS_FreeValue(ctx, p);
    return d;
}

static char *
ns_wa_str(JSContext *ctx, JSValueConst obj, const char *name)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    const char *s = JS_ToCString(ctx, v);
    char *r = g_strdup(s ? s : "");
    if (s) JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
    return r;
}

static float *
ns_wa_float32(JSContext *ctx, JSValueConst arr, uint32_t *count)
{
    *count = 0;
    if (!JS_IsObject(arr)) return NULL;
    size_t off = 0, blen = 0, bpe = 0, total = 0;
    JSValue buf = JS_GetTypedArrayBuffer(ctx, arr, &off, &blen, &bpe);
    if (JS_IsException(buf)) { JS_FreeValue(ctx, JS_GetException(ctx)); return NULL; }
    uint8_t *base = JS_GetArrayBuffer(ctx, &total, buf);
    JS_FreeValue(ctx, buf);
    if (!base || bpe != sizeof(float) || off + blen > total) return NULL;
    if ((off % sizeof(float)) != 0) return NULL;
    *count = (uint32_t)(blen / sizeof(float));
    return (float *)(void *)(base + off);
}

static void
ns_wa_window(JSContext *ctx, JSValueConst node, uint32_t frames, double rate,
             uint32_t *first, uint32_t *last)
{
    double start = ns_wa_num(ctx, node, "_startTime", -1.0);
    double stop = ns_wa_num(ctx, node, "_stopTime", -1.0);
    *first = 0;
    *last = frames;
    if (start > 0) {
        double f = start * rate;
        *first = f >= frames ? frames : (uint32_t)f;
    }
    if (stop >= 0) {
        double f = stop * rate;
        uint32_t e = f >= frames ? frames : (uint32_t)f;
        if (e < *last) *last = e;
    }
    if (*last < *first) *last = *first;
}

static void
ns_wa_oscillator(JSContext *ctx, JSValueConst node, uint32_t frames,
                 double rate, float *out)
{
    double freq = ns_wa_param(ctx, node, "frequency", 440.0);
    double detune = ns_wa_param(ctx, node, "detune", 0.0);
    double f = freq * pow(2.0, detune / 1200.0);
    if (!(f > 0) || f > rate * 0.5) f = f > 0 ? rate * 0.5 : 0.0;
    char *type = ns_wa_str(ctx, node, "type");
    uint32_t first, last;
    ns_wa_window(ctx, node, frames, rate, &first, &last);
    double step = f / rate, phase = 0.0;
    for (uint32_t i = first; i < last; i++) {
        double v;
        if (strcmp(type, "square") == 0)
            v = phase < 0.5 ? 1.0 : -1.0;
        else if (strcmp(type, "sawtooth") == 0)
            v = 2.0 * phase - 1.0;
        else if (strcmp(type, "triangle") == 0)
            v = phase < 0.5 ? 4.0 * phase - 1.0 : 3.0 - 4.0 * phase;
        else
            v = sin(2.0 * G_PI * phase);
        out[i] = (float)v;
        phase += step;
        if (phase >= 1.0) phase -= floor(phase);
    }
    g_free(type);
}

static void
ns_wa_buffer_source(JSContext *ctx, JSValueConst node, uint32_t frames,
                    double rate, float *out)
{
    JSValue buf = JS_GetPropertyStr(ctx, node, "buffer");
    if (!JS_IsObject(buf)) { JS_FreeValue(ctx, buf); return; }
    JSValue chans = JS_GetPropertyStr(ctx, buf, "_chans");
    JSValue ch0 = JS_GetPropertyUint32(ctx, chans, 0);
    uint32_t n = 0;
    const float *src = ns_wa_float32(ctx, ch0, &n);
    if (src && n) {
        double ratio = ns_wa_param(ctx, node, "playbackRate", 1.0);
        double brate = ns_wa_num(ctx, buf, "sampleRate", rate);
        if (!(ratio > 0)) ratio = 1.0;
        double step = ratio * (brate > 0 ? brate / rate : 1.0);
        JSValue loop_v = JS_GetPropertyStr(ctx, node, "loop");
        gboolean loop = JS_ToBool(ctx, loop_v);
        JS_FreeValue(ctx, loop_v);
        uint32_t first, last;
        ns_wa_window(ctx, node, frames, rate, &first, &last);
        double pos = 0.0;
        for (uint32_t i = first; i < last; i++) {
            if (pos >= n) {
                if (!loop) break;
                pos = fmod(pos, (double)n);
            }
            out[i] = src[(uint32_t)pos];
            pos += step;
        }
    }
    JS_FreeValue(ctx, ch0);
    JS_FreeValue(ctx, chans);
    JS_FreeValue(ctx, buf);
}

static void
ns_wa_sum_inputs(JSContext *ctx, JSValueConst node, uint32_t frames,
                 double rate, float *out, int depth)
{
    JSValue ins = JS_GetPropertyStr(ctx, node, "_inputs");
    if (!JS_IsObject(ins)) { JS_FreeValue(ctx, ins); return; }
    uint32_t n = ns_js_array_length(ctx, ins);
    if (!n) { JS_FreeValue(ctx, ins); return; }
    float *tmp = g_new0(float, frames);
    for (uint32_t i = 0; i < n; i++) {
        JSValue src = JS_GetPropertyUint32(ctx, ins, i);
        if (JS_IsObject(src)) {
            memset(tmp, 0, frames * sizeof(float));
            ns_wa_render(ctx, src, frames, rate, tmp, depth + 1);
            for (uint32_t j = 0; j < frames; j++) out[j] += tmp[j];
        }
        JS_FreeValue(ctx, src);
    }
    g_free(tmp);
    JS_FreeValue(ctx, ins);
}

static void
ns_wa_compressor(JSContext *ctx, JSValueConst node, uint32_t frames,
                 double rate, float *out)
{
    double threshold = ns_wa_param(ctx, node, "threshold", -24.0);
    double knee = ns_wa_param(ctx, node, "knee", 30.0);
    double ratio = ns_wa_param(ctx, node, "ratio", 12.0);
    double attack = ns_wa_param(ctx, node, "attack", 0.003);
    double release = ns_wa_param(ctx, node, "release", 0.25);
    if (ratio < 1.0) ratio = 1.0;
    if (knee < 0.0) knee = 0.0;
    double atk = attack > 0 ? exp(-1.0 / (attack * rate)) : 0.0;
    double rel = release > 0 ? exp(-1.0 / (release * rate)) : 0.0;
    double env = 0.0;
    for (uint32_t i = 0; i < frames; i++) {
        double x = fabs((double)out[i]);
        double db = x > 1e-9 ? 20.0 * log10(x) : -180.0;
        double over = db - threshold;
        double reduction;
        if (knee > 0 && over > -knee * 0.5 && over < knee * 0.5) {
            double t = over + knee * 0.5;
            reduction = (1.0 / ratio - 1.0) * t * t / (2.0 * knee);
        } else if (over <= 0) {
            reduction = 0.0;
        } else {
            reduction = over * (1.0 / ratio - 1.0);
        }
        double coeff = reduction < env ? atk : rel;
        env = coeff * env + (1.0 - coeff) * reduction;
        out[i] = (float)((double)out[i] * pow(10.0, env / 20.0));
    }
}

static void
ns_wa_biquad(JSContext *ctx, JSValueConst node, uint32_t frames,
             double rate, float *out)
{
    double f0 = ns_wa_param(ctx, node, "frequency", 350.0);
    double q = ns_wa_param(ctx, node, "Q", 1.0);
    double gain_db = ns_wa_param(ctx, node, "gain", 0.0);
    char *type = ns_wa_str(ctx, node, "type");
    if (!(f0 > 0)) f0 = 350.0;
    if (f0 > rate * 0.5) f0 = rate * 0.5;
    if (!(q > 0)) q = 1e-4;
    double w0 = 2.0 * G_PI * f0 / rate;
    double cw = cos(w0), sw = sin(w0), alpha = sw / (2.0 * q);
    double a0, a1, a2, b0, b1, b2;
    if (strcmp(type, "highpass") == 0) {
        b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
    } else if (strcmp(type, "bandpass") == 0) {
        b0 = alpha; b1 = 0; b2 = -alpha;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
    } else if (strcmp(type, "notch") == 0) {
        b0 = 1; b1 = -2 * cw; b2 = 1;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
    } else if (strcmp(type, "allpass") == 0) {
        b0 = 1 - alpha; b1 = -2 * cw; b2 = 1 + alpha;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
    } else if (strcmp(type, "peaking") == 0) {
        double A = pow(10.0, gain_db / 40.0);
        b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
    } else {
        b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2;
        a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
    }
    g_free(type);
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (uint32_t i = 0; i < frames; i++) {
        double x = out[i];
        double y = (b0 / a0) * x + (b1 / a0) * x1 + (b2 / a0) * x2
                 - (a1 / a0) * y1 - (a2 / a0) * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        out[i] = (float)y;
    }
}

static void
ns_wa_delay(JSContext *ctx, JSValueConst node, uint32_t frames,
            double rate, float *out)
{
    double t = ns_wa_param(ctx, node, "delayTime", 0.0);
    if (!(t > 0)) return;
    uint32_t d = (uint32_t)(t * rate);
    if (d == 0 || d >= frames) {
        if (d >= frames) memset(out, 0, frames * sizeof(float));
        return;
    }
    for (uint32_t i = frames; i-- > d;) out[i] = out[i - d];
    memset(out, 0, d * sizeof(float));
}

static void
ns_wa_waveshaper(JSContext *ctx, JSValueConst node, uint32_t frames,
                 float *out)
{
    JSValue curve = JS_GetPropertyStr(ctx, node, "curve");
    uint32_t n = 0;
    const float *c = ns_wa_float32(ctx, curve, &n);
    if (c && n >= 2) {
        for (uint32_t i = 0; i < frames; i++) {
            double x = out[i];
            if (x < -1.0) x = -1.0;
            if (x > 1.0) x = 1.0;
            double pos = (x + 1.0) * 0.5 * (n - 1);
            uint32_t k = (uint32_t)pos;
            if (k >= n - 1) { out[i] = c[n - 1]; continue; }
            double frac = pos - k;
            out[i] = (float)(c[k] * (1.0 - frac) + c[k + 1] * frac);
        }
    }
    JS_FreeValue(ctx, curve);
}

static void
ns_wa_render(JSContext *ctx, JSValueConst node, uint32_t frames,
             double rate, float *out, int depth)
{
    if (depth > NS_WA_MAX_DEPTH || !JS_IsObject(node)) return;
    char *kind = ns_wa_str(ctx, node, "_kind");

    if (strcmp(kind, "oscillator") == 0) {
        ns_wa_oscillator(ctx, node, frames, rate, out);
    } else if (strcmp(kind, "buffersource") == 0) {
        ns_wa_buffer_source(ctx, node, frames, rate, out);
    } else if (strcmp(kind, "constant") == 0) {
        double v = ns_wa_param(ctx, node, "offset", 1.0);
        uint32_t first, last;
        ns_wa_window(ctx, node, frames, rate, &first, &last);
        for (uint32_t i = first; i < last; i++) out[i] = (float)v;
    } else {
        ns_wa_sum_inputs(ctx, node, frames, rate, out, depth);
        if (strcmp(kind, "gain") == 0) {
            double g = ns_wa_param(ctx, node, "gain", 1.0);
            for (uint32_t i = 0; i < frames; i++) out[i] = (float)(out[i] * g);
        } else if (strcmp(kind, "compressor") == 0) {
            ns_wa_compressor(ctx, node, frames, rate, out);
        } else if (strcmp(kind, "biquad") == 0) {
            ns_wa_biquad(ctx, node, frames, rate, out);
        } else if (strcmp(kind, "delay") == 0) {
            ns_wa_delay(ctx, node, frames, rate, out);
        } else if (strcmp(kind, "waveshaper") == 0) {
            ns_wa_waveshaper(ctx, node, frames, out);
        } else if (strcmp(kind, "stereopanner") == 0 ||
                   strcmp(kind, "panner") == 0) {
            double g = 1.0 - fabs(ns_wa_param(ctx, node, "pan", 0.0)) * 0.5;
            for (uint32_t i = 0; i < frames; i++) out[i] = (float)(out[i] * g);
        }
    }
    g_free(kind);
}

gboolean
ns_webaudio_render_offline(JSContext *ctx, JSValueConst destination,
                           uint32_t frames, double rate, float *out)
{
    if (!ctx || !out || !frames || !(rate > 0)) return FALSE;
    memset(out, 0, frames * sizeof(float));
    if (!JS_IsObject(destination)) return FALSE;
    ns_wa_render(ctx, destination, frames, rate, out, 0);
    for (uint32_t i = 0; i < frames; i++) {
        if (out[i] > 1.0f) out[i] = 1.0f;
        else if (out[i] < -1.0f) out[i] = -1.0f;
    }
    return TRUE;
}
