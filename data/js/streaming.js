/* Nordstjernen — HLS and DASH playback over Media Source Extensions.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

(function (global) {
    'use strict';

    if (!global || global.__ndStreamingInstalled) return;
    if (typeof global.MediaSource !== 'function' ||
        typeof global.fetch !== 'function' ||
        typeof global.URL !== 'function')
        return;
    global.__ndStreamingInstalled = true;

    var HLS_EXT = /\.m3u8(\?|#|$)/i;
    var DASH_EXT = /\.mpd(\?|#|$)/i;
    var HLS_TYPES = ['application/x-mpegurl', 'application/vnd.apple.mpegurl',
                     'audio/x-mpegurl', 'audio/mpegurl'];
    var DASH_TYPES = ['application/dash+xml'];
    var TARGET_BUFFER_SECONDS = 30;
    var LIVE_REFRESH_MIN_MS = 2000;

    function absolute(url, base) {
        try { return new global.URL(url, base || global.location.href).href; }
        catch (e) { return url; }
    }

    function fetchText(url) {
        return global.fetch(url, { credentials: 'include' })
            .then(function (r) {
                if (!r.ok) throw new Error('HTTP ' + r.status + ' for ' + url);
                return r.text().then(function (body) {
                    return { body: body, url: r.url || url };
                });
            });
    }

    function fetchBytes(url, range) {
        var init = { credentials: 'include' };
        if (range) init.headers = { Range: 'bytes=' + range };
        return global.fetch(url, init).then(function (r) {
            if (!r.ok) throw new Error('HTTP ' + r.status + ' for ' + url);
            return r.arrayBuffer();
        });
    }

    function mimeFor(kind, container, codecs) {
        var type = (kind === 'audio' ? 'audio/' : 'video/') + container;
        return codecs ? type + '; codecs="' + codecs + '"' : type;
    }

    function attrList(line) {
        var out = {};
        var re = /([A-Z0-9-]+)=("[^"]*"|[^,]*)/g;
        var m;
        while ((m = re.exec(line)) !== null) {
            var value = m[2];
            if (value.charAt(0) === '"') value = value.slice(1, -1);
            out[m[1]] = value;
        }
        return out;
    }

    function parseMasterPlaylist(text, base) {
        var lines = text.split(/\r?\n/);
        var variants = [];
        var renditions = [];
        for (var i = 0; i < lines.length; i++) {
            var line = lines[i].trim();
            if (line.indexOf('#EXT-X-STREAM-INF:') === 0) {
                var attrs = attrList(line.slice(18));
                var uri = '';
                for (var j = i + 1; j < lines.length; j++) {
                    var next = lines[j].trim();
                    if (!next || next.charAt(0) === '#') continue;
                    uri = next;
                    i = j;
                    break;
                }
                if (!uri) continue;
                var res = String(attrs.RESOLUTION || '').split('x');
                variants.push({
                    url: absolute(uri, base),
                    bandwidth: parseInt(attrs['AVERAGE-BANDWIDTH'] ||
                                        attrs.BANDWIDTH, 10) || 0,
                    width: parseInt(res[0], 10) || 0,
                    height: parseInt(res[1], 10) || 0,
                    codecs: attrs.CODECS || '',
                    audioGroup: attrs.AUDIO || ''
                });
            } else if (line.indexOf('#EXT-X-MEDIA:') === 0) {
                var media = attrList(line.slice(13));
                if (media.TYPE !== 'AUDIO' || !media.URI) continue;
                renditions.push({
                    url: absolute(media.URI, base),
                    group: media.GROUP_ID || media['GROUP-ID'] || '',
                    isDefault: media.DEFAULT === 'YES',
                    language: media.LANGUAGE || ''
                });
            }
        }
        return { variants: variants, renditions: renditions };
    }

    function parseMediaPlaylist(text, base) {
        var lines = text.split(/\r?\n/);
        var segments = [];
        var duration = 0;
        var pendingDuration = 0;
        var pendingRange = '';
        var initSegment = null;
        var live = true;
        var targetDuration = 0;
        var startSequence = 0;
        for (var i = 0; i < lines.length; i++) {
            var line = lines[i].trim();
            if (!line) continue;
            if (line.charAt(0) === '#') {
                if (line.indexOf('#EXTINF:') === 0) {
                    pendingDuration = parseFloat(line.slice(8)) || 0;
                } else if (line.indexOf('#EXT-X-BYTERANGE:') === 0) {
                    pendingRange = line.slice(17).trim();
                } else if (line.indexOf('#EXT-X-MAP:') === 0) {
                    var map = attrList(line.slice(11));
                    if (map.URI)
                        initSegment = {
                            url: absolute(map.URI, base),
                            range: byteRange(map.BYTERANGE)
                        };
                } else if (line.indexOf('#EXT-X-TARGETDURATION:') === 0) {
                    targetDuration = parseFloat(line.slice(22)) || 0;
                } else if (line.indexOf('#EXT-X-MEDIA-SEQUENCE:') === 0) {
                    startSequence = parseInt(line.slice(22), 10) || 0;
                } else if (line.indexOf('#EXT-X-ENDLIST') === 0) {
                    live = false;
                }
                continue;
            }
            segments.push({
                url: absolute(line, base),
                duration: pendingDuration,
                start: duration,
                range: byteRange(pendingRange),
                sequence: startSequence + segments.length
            });
            duration += pendingDuration;
            pendingDuration = 0;
            pendingRange = '';
        }
        return {
            segments: segments, duration: duration, live: live,
            initSegment: initSegment, targetDuration: targetDuration
        };
    }

    function byteRange(spec) {
        if (!spec) return '';
        var parts = String(spec).split('@');
        var length = parseInt(parts[0], 10);
        var offset = parts.length > 1 ? parseInt(parts[1], 10) : 0;
        if (!(length > 0)) return '';
        return offset + '-' + (offset + length - 1);
    }

    function parseDuration(text) {
        if (!text) return 0;
        var m = /^P(?:([\d.]+)Y)?(?:([\d.]+)M)?(?:([\d.]+)D)?(?:T(?:([\d.]+)H)?(?:([\d.]+)M)?(?:([\d.]+)S)?)?$/
            .exec(String(text).trim());
        if (!m) return 0;
        return (parseFloat(m[1]) || 0) * 31536000 +
               (parseFloat(m[2]) || 0) * 2592000 +
               (parseFloat(m[3]) || 0) * 86400 +
               (parseFloat(m[4]) || 0) * 3600 +
               (parseFloat(m[5]) || 0) * 60 +
               (parseFloat(m[6]) || 0);
    }

    function expandTemplate(template, values) {
        return String(template).replace(
            /\$(\$|RepresentationID|Number|Bandwidth|Time)(?:%0(\d+)d)?\$/g,
            function (all, name, width) {
                if (name === '$') return '$';
                var value = values[name];
                if (value === undefined || value === null) return all;
                value = String(value);
                if (width) while (value.length < Number(width))
                    value = '0' + value;
                return value;
            });
    }

    function childrenNamed(node, name) {
        var out = [];
        if (!node || !node.children) return out;
        for (var i = 0; i < node.children.length; i++) {
            var child = node.children[i];
            var tag = child.tagName || '';
            if (tag === name || tag.slice(-(name.length + 1)) === ':' + name)
                out.push(child);
        }
        return out;
    }

    function firstNamed(node, name) {
        return childrenNamed(node, name)[0] || null;
    }

    function inherited(node, name) {
        while (node) {
            if (node.getAttribute) {
                var value = node.getAttribute(name);
                if (value !== null && value !== '') return value;
            }
            node = node.parentNode;
        }
        return '';
    }

    function segmentTemplateFor(representation) {
        var node = representation;
        while (node) {
            var template = firstNamed(node, 'SegmentTemplate');
            if (template) return template;
            node = node.parentNode;
        }
        return null;
    }

    function parseDash(text, base) {
        if (typeof global.DOMParser !== 'function') return null;
        var doc;
        try { doc = new global.DOMParser().parseFromString(text, 'text/xml'); }
        catch (e) { return null; }
        var mpd = doc && doc.documentElement;
        if (!mpd) return null;
        var baseUrlNode = firstNamed(mpd, 'BaseURL');
        var root = baseUrlNode && baseUrlNode.textContent
            ? absolute(baseUrlNode.textContent.trim(), base) : base;
        var live = (mpd.getAttribute('type') || 'static') === 'dynamic';
        var duration = parseDuration(mpd.getAttribute('mediaPresentationDuration'));
        var period = firstNamed(mpd, 'Period');
        if (!period) return null;
        var sets = childrenNamed(period, 'AdaptationSet');
        var tracks = { video: [], audio: [] };
        for (var i = 0; i < sets.length; i++) {
            var set = sets[i];
            var contentType = set.getAttribute('contentType') || '';
            var setMime = set.getAttribute('mimeType') || '';
            var reps = childrenNamed(set, 'Representation');
            for (var j = 0; j < reps.length; j++) {
                var rep = reps[j];
                var mime = rep.getAttribute('mimeType') || setMime;
                var kind = contentType ||
                    (mime.indexOf('audio/') === 0 ? 'audio' : 'video');
                if (kind !== 'audio' && kind !== 'video') continue;
                var track = dashRepresentation(rep, set, mime, root, duration);
                if (track) tracks[kind].push(track);
            }
        }
        return {
            video: tracks.video, audio: tracks.audio,
            duration: duration, live: live
        };
    }

    function dashRepresentation(rep, set, mime, base, presentationDuration) {
        var template = segmentTemplateFor(rep);
        if (!template) return null;
        var id = rep.getAttribute('id') || '';
        var bandwidth = parseInt(rep.getAttribute('bandwidth'), 10) || 0;
        var codecs = rep.getAttribute('codecs') ||
                     set.getAttribute('codecs') || '';
        var timescale = parseInt(template.getAttribute('timescale'), 10) || 1;
        var startNumber = parseInt(template.getAttribute('startNumber'), 10);
        if (isNaN(startNumber)) startNumber = 1;
        var values = { RepresentationID: id, Bandwidth: bandwidth };
        var initTemplate = template.getAttribute('initialization') ||
                           inherited(rep, 'initialization');
        var mediaTemplate = template.getAttribute('media') || '';
        if (!mediaTemplate) return null;

        var segments = [];
        var timeline = firstNamed(template, 'SegmentTimeline');
        if (timeline) {
            var entries = childrenNamed(timeline, 'S');
            var time = 0;
            var number = startNumber;
            for (var i = 0; i < entries.length; i++) {
                var entry = entries[i];
                var t = entry.getAttribute('t');
                if (t !== null && t !== '') time = parseFloat(t) || 0;
                var d = parseFloat(entry.getAttribute('d')) || 0;
                var repeat = parseInt(entry.getAttribute('r'), 10) || 0;
                for (var k = 0; k <= repeat; k++) {
                    values.Number = number;
                    values.Time = time;
                    segments.push({
                        url: absolute(expandTemplate(mediaTemplate, values), base),
                        start: time / timescale,
                        duration: d / timescale,
                        range: ''
                    });
                    time += d;
                    number++;
                }
            }
        } else {
            var segDuration = parseFloat(template.getAttribute('duration')) || 0;
            if (!(segDuration > 0) || !(presentationDuration > 0)) return null;
            var seconds = segDuration / timescale;
            var count = Math.ceil(presentationDuration / seconds);
            for (var n = 0; n < count; n++) {
                values.Number = startNumber + n;
                values.Time = n * segDuration;
                segments.push({
                    url: absolute(expandTemplate(mediaTemplate, values), base),
                    start: n * seconds,
                    duration: seconds,
                    range: ''
                });
            }
        }
        if (!segments.length) return null;

        var container = mime.indexOf('webm') >= 0 ? 'webm' : 'mp4';
        values.Number = startNumber;
        values.Time = 0;
        return {
            bandwidth: bandwidth,
            width: parseInt(rep.getAttribute('width'), 10) || 0,
            height: parseInt(rep.getAttribute('height'), 10) || 0,
            codecs: codecs,
            container: container,
            initSegment: initTemplate
                ? { url: absolute(expandTemplate(initTemplate, values), base),
                    range: '' }
                : null,
            segments: segments
        };
    }

    function pickRendition(list, kind) {
        var best = null;
        for (var i = 0; i < list.length; i++) {
            var item = list[i];
            var mime = mimeFor(kind, item.container || 'mp4', item.codecs);
            if (item.codecs && !global.MediaSource.isTypeSupported(mime))
                continue;
            if (!best || preferred(item, best)) best = item;
        }
        return best;
    }

    function preferred(candidate, current) {
        var capped = candidate.height && candidate.height <= 1080;
        var currentCapped = current.height && current.height <= 1080;
        if (capped !== currentCapped) return capped;
        return (candidate.bandwidth || 0) > (current.bandwidth || 0);
    }

    function StreamSession(element, url, kind) {
        this.element = element;
        this.url = url;
        this.kind = kind;
        this.mediaSource = new global.MediaSource();
        this.tracks = [];
        this.stopped = false;
        this.refreshTimer = 0;
    }

    StreamSession.prototype.start = function () {
        var self = this;
        var loader = this.kind === 'hls' ? this.loadHls() : this.loadDash();
        return loader.then(function () {
            if (self.stopped || !self.tracks.length)
                throw new Error('no playable rendition in ' + self.url);
            return self.attach();
        }).catch(function (err) {
            self.fail(err);
        });
    };

    StreamSession.prototype.fail = function (err) {
        this.stop();
        try { console.warn('streaming: ' + (err && err.message || err)); }
        catch (e) {}
        try { this.element.dispatchEvent(new global.Event('error')); }
        catch (e) {}
    };

    StreamSession.prototype.stop = function () {
        this.stopped = true;
        if (this.refreshTimer) {
            global.clearTimeout(this.refreshTimer);
            this.refreshTimer = 0;
        }
    };

    StreamSession.prototype.loadHls = function () {
        var self = this;
        return fetchText(this.url).then(function (res) {
            var master = parseMasterPlaylist(res.body, res.url);
            if (!master.variants.length) {
                var media = parseMediaPlaylist(res.body, res.url);
                if (!media.segments.length)
                    throw new Error('empty HLS playlist');
                self.live = media.live;
                self.duration = media.duration;
                self.tracks = [self.makeTrack('video', media, '', res.url)];
                return null;
            }
            var variant = pickHlsVariant(master.variants);
            if (!variant) throw new Error('no supported HLS variant');
            var audio = pickHlsAudio(master.renditions, variant.audioGroup);
            var codecs = splitCodecs(variant.codecs);
            var jobs = [fetchText(variant.url).then(function (r) {
                var media = parseMediaPlaylist(r.body, r.url);
                if (!media.segments.length)
                    throw new Error('empty HLS media playlist');
                self.live = media.live;
                self.duration = media.duration;
                self.tracks.push(self.makeTrack('video', media,
                    audio ? codecs.video : variant.codecs, r.url));
            })];
            if (audio) {
                jobs.push(fetchText(audio.url).then(function (r) {
                    var media = parseMediaPlaylist(r.body, r.url);
                    if (!media.segments.length) return;
                    self.tracks.push(self.makeTrack('audio', media,
                                                    codecs.audio, r.url));
                }).catch(function () {}));
            }
            return global.Promise.all(jobs);
        });
    };

    StreamSession.prototype.makeTrack = function (kind, media, codecs, url) {
        return {
            kind: kind,
            url: url,
            codecs: codecs,
            container: containerForCodecs(codecs, media),
            initSegment: media.initSegment,
            segments: media.segments,
            targetDuration: media.targetDuration,
            nextIndex: 0,
            appendedInit: false,
            sourceBuffer: null
        };
    };

    StreamSession.prototype.loadDash = function () {
        var self = this;
        return fetchText(this.url).then(function (res) {
            var mpd = parseDash(res.body, res.url);
            if (!mpd) throw new Error('unparsable MPD');
            self.live = mpd.live;
            self.duration = mpd.duration;
            var video = pickRendition(mpd.video, 'video');
            var audio = pickRendition(mpd.audio, 'audio');
            if (video) self.tracks.push(dashTrack(video, 'video'));
            if (audio) self.tracks.push(dashTrack(audio, 'audio'));
        });
    };

    function dashTrack(rep, kind) {
        return {
            kind: kind,
            codecs: rep.codecs,
            container: rep.container,
            initSegment: rep.initSegment,
            segments: rep.segments,
            nextIndex: 0,
            appendedInit: false,
            sourceBuffer: null
        };
    }

    function containerForCodecs(codecs, media) {
        if (/vp0?[89]|av01|vorbis|opus/i.test(codecs || '')) return 'webm';
        if (media && media.initSegment) return 'mp4';
        if (media && media.segments.length &&
            /\.(ts|m2ts)(\?|#|$)/i.test(media.segments[0].url))
            return 'mp2t';
        return 'mp4';
    }

    function splitCodecs(codecs) {
        var out = { video: '', audio: '' };
        String(codecs || '').split(',').forEach(function (raw) {
            var codec = raw.trim();
            if (!codec) return;
            if (/^(avc|hev|hvc|vp0?[89]|av01|vvc)/i.test(codec)) out.video = codec;
            else out.audio = codec;
        });
        return out;
    }

    function pickHlsVariant(variants) {
        var best = null;
        for (var i = 0; i < variants.length; i++) {
            var variant = variants[i];
            var codecs = splitCodecs(variant.codecs);
            if (variant.codecs) {
                var mime = mimeFor('video',
                    /vp0?[89]|av01/i.test(variant.codecs) ? 'webm' : 'mp4',
                    variant.codecs);
                if (!global.MediaSource.isTypeSupported(mime) &&
                    !global.MediaSource.isTypeSupported(
                        mimeFor('video', 'mp4', codecs.video)))
                    continue;
            }
            if (!best || preferred(variant, best)) best = variant;
        }
        return best;
    }

    function pickHlsAudio(renditions, group) {
        for (var i = 0; i < renditions.length; i++)
            if (!group || renditions[i].group === group)
                if (renditions[i].isDefault) return renditions[i];
        for (var j = 0; j < renditions.length; j++)
            if (!group || renditions[j].group === group) return renditions[j];
        return null;
    }

    StreamSession.prototype.attach = function () {
        var self = this;
        var ms = this.mediaSource;
        var objectUrl = global.URL.createObjectURL(ms);
        this.element.src = objectUrl;
        return new global.Promise(function (resolve, reject) {
            ms.addEventListener('sourceopen', function () {
                try {
                    self.openBuffers();
                    if (self.duration > 0) ms.duration = self.duration;
                } catch (err) { reject(err); return; }
                self.pump();
                resolve();
            });
        });
    };

    StreamSession.prototype.openBuffers = function () {
        for (var i = 0; i < this.tracks.length; i++) {
            var track = this.tracks[i];
            var mime = mimeFor(track.kind, track.container, track.codecs);
            if (!global.MediaSource.isTypeSupported(mime))
                mime = mimeFor(track.kind, track.container, '');
            track.sourceBuffer = this.mediaSource.addSourceBuffer(mime);
        }
    };

    StreamSession.prototype.pump = function () {
        var self = this;
        if (this.stopped) return;
        var pending = this.tracks.map(function (track) {
            return self.pumpTrack(track);
        });
        global.Promise.all(pending).then(function () {
            if (self.stopped) return;
            if (self.tracksExhausted()) {
                if (self.live) self.scheduleRefresh();
                else self.finish();
                return;
            }
            self.pump();
        }).catch(function (err) { self.fail(err); });
    };

    StreamSession.prototype.tracksExhausted = function () {
        for (var i = 0; i < this.tracks.length; i++)
            if (this.tracks[i].nextIndex < this.tracks[i].segments.length)
                return false;
        return true;
    };

    StreamSession.prototype.finish = function () {
        try {
            if (this.mediaSource.readyState === 'open')
                this.mediaSource.endOfStream();
        } catch (e) {}
    };

    StreamSession.prototype.scheduleRefresh = function () {
        var self = this;
        var track = this.tracks[0];
        var wait = Math.max(LIVE_REFRESH_MIN_MS,
                            (track && track.targetDuration || 4) * 1000 / 2);
        this.refreshTimer = global.setTimeout(function () {
            self.refreshTimer = 0;
            self.refresh();
        }, wait);
    };

    StreamSession.prototype.refresh = function () {
        var self = this;
        if (this.stopped || this.kind !== 'hls') return;
        var jobs = this.tracks.map(function (track) {
            if (!track.url) return null;
            return fetchText(track.url).then(function (r) {
                var media = parseMediaPlaylist(r.body, r.url);
                mergeSegments(track, media.segments);
                if (!media.live) self.live = false;
            });
        }).filter(Boolean);
        global.Promise.all(jobs).then(function () {
            if (!self.stopped) self.pump();
        }).catch(function (err) { self.fail(err); });
    };

    function mergeSegments(track, incoming) {
        if (!incoming.length) return;
        var known = track.segments.length
            ? track.segments[track.segments.length - 1].sequence : -1;
        for (var i = 0; i < incoming.length; i++)
            if (incoming[i].sequence > known) track.segments.push(incoming[i]);
    }

    StreamSession.prototype.bufferAhead = function (track) {
        var buffered = track.sourceBuffer && track.sourceBuffer.buffered;
        if (!buffered || !buffered.length) return 0;
        return buffered.end(buffered.length - 1) - (this.element.currentTime || 0);
    };

    StreamSession.prototype.pumpTrack = function (track) {
        var self = this;
        if (this.stopped || !track.sourceBuffer) return global.Promise.resolve();
        if (track.nextIndex >= track.segments.length)
            return global.Promise.resolve();
        if (this.bufferAhead(track) > TARGET_BUFFER_SECONDS)
            return delay(250);
        var job = global.Promise.resolve();
        if (track.initSegment && !track.appendedInit) {
            track.appendedInit = true;
            job = fetchBytes(track.initSegment.url, track.initSegment.range)
                .then(function (bytes) { return self.append(track, bytes); });
        }
        var segment = track.segments[track.nextIndex++];
        return job.then(function () {
            return fetchBytes(segment.url, segment.range);
        }).then(function (bytes) {
            return self.append(track, bytes);
        });
    };

    StreamSession.prototype.append = function (track, bytes) {
        var self = this;
        var sb = track.sourceBuffer;
        return new global.Promise(function (resolve, reject) {
            function done() { cleanup(); resolve(); }
            function failed() { cleanup(); reject(new Error('append failed')); }
            function cleanup() {
                sb.removeEventListener('updateend', done);
                sb.removeEventListener('error', failed);
            }
            sb.addEventListener('updateend', done);
            sb.addEventListener('error', failed);
            try {
                sb.appendBuffer(new Uint8Array(bytes));
            } catch (err) {
                cleanup();
                if (err && err.name === 'QuotaExceededError') {
                    self.evict(track).then(resolve, reject);
                    return;
                }
                reject(err);
            }
        });
    };

    StreamSession.prototype.evict = function (track) {
        var sb = track.sourceBuffer;
        var buffered = sb.buffered;
        if (!buffered || !buffered.length) return global.Promise.resolve();
        var keepFrom = Math.max(0, (this.element.currentTime || 0) - 10);
        if (keepFrom <= buffered.start(0)) return global.Promise.resolve();
        return new global.Promise(function (resolve) {
            function done() {
                sb.removeEventListener('updateend', done);
                resolve();
            }
            sb.addEventListener('updateend', done);
            try { sb.remove(buffered.start(0), keepFrom); }
            catch (e) { done(); }
        });
    };

    function delay(ms) {
        return new global.Promise(function (resolve) {
            global.setTimeout(resolve, ms);
        });
    }

    function streamKindFor(element) {
        var src = '';
        try { src = element.currentSrc || element.src || ''; } catch (e) {}
        var type = '';
        try { type = String(element.getAttribute('type') || '').toLowerCase(); }
        catch (e) {}
        if (!src) {
            var sources = element.getElementsByTagName
                ? element.getElementsByTagName('source') : [];
            for (var i = 0; i < sources.length; i++) {
                var candidate = sources[i].getAttribute('src') || '';
                var candidateType =
                    String(sources[i].getAttribute('type') || '').toLowerCase();
                var kind = kindFor(candidate, candidateType);
                if (kind) return { kind: kind, src: absolute(candidate) };
            }
            return null;
        }
        var found = kindFor(src, type);
        return found ? { kind: found, src: absolute(src) } : null;
    }

    function kindFor(src, type) {
        var mime = type.split(';')[0].trim();
        if (HLS_TYPES.indexOf(mime) >= 0) return 'hls';
        if (DASH_TYPES.indexOf(mime) >= 0) return 'dash';
        if (HLS_EXT.test(src)) return 'hls';
        if (DASH_EXT.test(src)) return 'dash';
        return null;
    }

    var sessions = typeof global.WeakMap === 'function' ? new global.WeakMap()
                                                        : null;

    function adopt(element) {
        if (!element || element.__ndStreamOwned) return;
        var found = streamKindFor(element);
        if (!found) return;
        element.__ndStreamOwned = true;
        var session = new StreamSession(element, found.src, found.kind);
        if (sessions) sessions.set(element, session);
        session.start();
    }

    function scan(root) {
        if (!root || !root.querySelectorAll) return;
        var nodes;
        try { nodes = root.querySelectorAll('video,audio'); }
        catch (e) { return; }
        for (var i = 0; i < nodes.length; i++) adopt(nodes[i]);
    }

    var rescanPending = 0;

    function scheduleScan() {
        if (rescanPending) return;
        rescanPending = global.setTimeout(function () {
            rescanPending = 0;
            scan(global.document);
        }, 50);
    }

    function install() {
        scan(global.document);
        if (typeof global.MutationObserver === 'function') {
            try {
                new global.MutationObserver(scheduleScan).observe(
                    global.document.documentElement,
                    { childList: true, subtree: true,
                      attributes: true, attributeFilter: ['src'] });
                return;
            } catch (e) {}
        }
        global.setInterval(function () { scan(global.document); }, 1000);
    }

    if (!global.document) return;
    if (global.document.readyState === 'loading' &&
        typeof global.document.addEventListener === 'function')
        global.document.addEventListener('DOMContentLoaded', install);
    else
        install();
})(typeof globalThis !== 'undefined' ? globalThis : this);
