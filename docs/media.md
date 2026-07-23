# Media: video and audio

How Nordstjernen plays `<video>` and `<audio>`. The engine ships a tiny,
in-tree decoder set plus one optional WebM extension; anything else renders
a poster and a play overlay.

The video process handles video, the audio process handles audio 
and the html process renders html.

## Inline video

### MPEG-1 (always on)

`<video>` plays **inline** when the source is MPEG-1 (an `.mpg` / `.mpeg` /
`.m1v` stream). The bytes are decoded entirely in-tree by
[pl_mpeg](https://github.com/phoboslab/pl_mpeg) — a single-file, MIT-licensed
MPEG-1 video decoder — inside the sandboxed renderer process; no external
library, no GPU API, and no syscalls beyond memory are needed, so it runs
comfortably under the seccomp filter. Decoded BGRA frames are advanced off the
renderer's existing animation tick, honouring the `autoplay`, `loop`, `muted`,
`width`/`height`, and `poster` attributes; a click toggles play/pause; and the
`HTMLMediaElement` events (`loadedmetadata`, `durationchange`, `canplay`,
`timeupdate`, `play`, `pause`, `ended`) fire on the element as it plays. MPEG-1
is the one always-on codec, by design — small, patent-free, and decoded in pure
portable C.

### WebM — VP9/VP8 (required on Linux and Windows)

`.webm` plays inline too when **FFmpeg's libav\*** (`libavformat` /
`libavcodec` / `libavutil` / `libswscale` / `libswresample`) is present at build
time (`-DNS_HAVE_LIBAV`). These are the component libraries of
[FFmpeg](https://github.com/FFmpeg/FFmpeg) (canonical git
<https://git.ffmpeg.org/ffmpeg.git>) — an actively maintained project, **not**
the defunct [Libav](https://github.com/libav/libav) fork (last release 2018),
whose `libav.org` domain is unrelated despite the shared library-name prefix.
libav demuxes the Matroska container
and decodes **VP9/VP8** frames, which are scaled to BGRA and served through the
same off-tick frame loop as the MPEG-1 path — both royalty-free codecs. It is
**required on Linux and Windows** (`meson setup` fails without it, or with a
pre-8.0 FFmpeg) because YouTube and most modern sites serve VP9/WebM and the
poster-and-overlay fallback there is unacceptable; on **macOS** it is
auto-detected, and a build without the FFmpeg libraries carries no libav symbol
or dependency and falls back to the poster-and-overlay path. The minimum is
**FFmpeg 8.0** (library sonames libavcodec ≥ 62, libavformat ≥ 62, libavutil ≥
60, libswscale ≥ 9, libswresample ≥ 6).

How libav is obtained differs by platform, to keep it redistributable:

- **Linux** depends on the distribution's FFmpeg at runtime (never bundled), so
  no licensing obligation falls on the package.
- **macOS / Windows** vendor a **minimal, LGPL-only FFmpeg** built from source
  (`scripts/build-ffmpeg-lgpl.sh`), carrying just the matroska/ogg demuxers and
  the native VP8/VP9/Opus/Vorbis decoders — no GPL parts, no external codec
  libraries.
- **Android / iOS** do not cross-build FFmpeg, so the desktop libav path is
  unavailable. Instead the mobile dependency sysroot
  ([nordstjernen-dependencies-build](https://github.com/nordstjernen-web/nordstjernen-dependencies-build))
  cross-builds the standalone open-media decoders — **dav1d** (AV1), **libvpx**
  (VP8/VP9), **opus** and **vorbis**.

Those same standalone decoders are detected on **every** platform — mobile from
the sysroot above, desktop from system packages (`libdav1d-dev` / `libvpx-dev` /
`libopus-dev` / `libvorbis-dev`; Homebrew `dav1d`/`libvpx`/`opus`/`libvorbis`) —
and linked whenever present (`NS_HAVE_MEDIA_CODECS`; availability is queried
through `src/media_codecs.c`). So the **desktop** build benefits too: they
complement FFmpeg and give a native decode path where FFmpeg is absent (e.g. a
stock macOS build, whose WebM otherwise falls back to the external player). Until
the decode implementation is wired onto that seam, a build without libav still
uses the external-player route.

## Audio

The MPEG-1 stream's **MP2 audio track** plays too (unless the element is
`muted`), as does a WebM's **Opus/Vorbis** track when libav is built in. The
seccomp-sandboxed renderer can't open a sound device, so audio is handed to the
unsandboxed `nordstjernen-audio` helper. The helper decodes to PCM — pl_mpeg for
the MPEG-1/MP2 track, [minimp3](https://github.com/lieff/minimp3) (CC0, vendored)
for standalone `.mp3` files, and libav for Opus/Vorbis (WebM/Ogg) — and plays it
through [SDL2](https://www.libsdl.org/)'s audio device (WASAPI on Windows,
CoreAudio on macOS, ALSA/PulseAudio on Linux), mixing and resampling the streams
itself. The inline player drives the helper — `open`/`play`/`pause`/`seek`/`stop`
ride the renderer→shell render channel, and looping re-syncs the audio at each
wrap.

## Captions and subtitles (`<track>`)

A `<video>` may carry a `<track>` child pointing at a
[WebVTT](https://www.w3.org/TR/webvtt1/) file. When one is marked `default`
and its `kind` is `subtitles`/`captions` (or omitted — the missing-value
default), the renderer fetches it during video discovery
(`ns_video_discover_track` in `src/video.c`) and parses it into timed cues
(`ns_vtt_parse`): it reads `[HH:]MM:SS.mmm` start/end timestamps, skips the
`WEBVTT` header, `NOTE` comments, cue identifiers and the cue-setting suffix
after the end timestamp, strips inline `<…>` tags, decodes the common HTML
entities, and keeps multi-line cue text. Only the `default` track shows,
which is the spec's initial text-track mode; scripted `TextTrack.mode`
switching and cue positioning settings (`line`/`position`/`align`) are not
wired.

The cue active at the video's current time is drawn by `paint_video_caption`
(`src/paint.c`) as centred, white-on-translucent-black lines across the
bottom of the video box. The captions are painted into the **html page
surface**, after the video rectangle is punched out — and because the shell
composites the page surface *last, over* the video-process frame (see the
helper section below), the caption pixels land on top of the video in both
decode paths: the in-process frame texture and the helper's shared-memory
frames. Playback-time (`v->cur_time`) and the parsed cues are tracked
renderer-side either way, so the same captions appear regardless of who
decodes the picture.

## The video helper process (`nordstjernen-video`)

MSE streams (YouTube-style playback) decode in a **third process** that sits
beside the renderer and the audio helper. The renderer appends the growing MSE
byte stream to a file under `~/.cache/nordstjernen/msvideo/` (exactly as it
materializes the audio track for `nordstjernen-audio`) and drives the helper
over the same renderer→shell media channel with
`video open`/`reload`/`play`/`pause`/`seek`/`stop` lines, plus a
`video rect` line carrying the on-page rectangle of the `<video>` box captured
at paint time. The helper decodes with libav on its own presentation clock and
writes BGRA frames into a shared-memory ring (`/nsvid-<pid>-<n>`, three slots);
the UI shell maps the ring and **composites the newest frame over the page
surface** on every widget frame tick.

This decouples video from page rendering: a page repaint is only needed when
the page itself changes, while video advances at full rate in the compositor —
so large players no longer force full-page repaints per frame, decode jank
never blocks the renderer main loop, and attacker-controlled codec bytes are
parsed in a small self-sandboxed process (Landlock on Linux) that can reach
only its shm ring and temp files. The helper is optional: it is built when
libav is available — including on Windows, where the shared-memory ring is a
named file mapping (`CreateFileMapping`/`MapViewOfFile`) rather than POSIX
`shm_open` — and when the binary is missing, or in headless mode, the renderer
decodes frames in-process exactly as before. The renderer always keeps the demuxer state that backs
`buffered`/`duration`/`currentTime`, so page JS sees the same element state
either way; audio stays in `nordstjernen-audio`, cued by the same
play/pause/seek commands so both clocks anchor identically.

Each appended media segment is tracked with its byte offset and probed time
range. When a player evicts an old prefix with `SourceBuffer.remove()`, the
browser rebuilds the helper input from the initialization segment and retained
media segments. This keeps long-running MSE playback within the byte quota used
by adaptive players such as YouTube and Vimeo. The retained segment's first and
last demux timestamps are returned through `SourceBuffer.buffered`, so player
eviction and seek decisions no longer see a synthetic zero-based range.

## Other media

Beyond MPEG-1/MP2, MP3, and the optional WebM (VP9/VP8 + Opus/Vorbis) path,
Nordstjernen ships no media codecs. Other `<audio>` and other `<video>` codecs
render a poster and a play overlay instead of playing. Clicking one resolves
the source URL inside the sandboxed renderer process (`ns_browser_media_at`),
and the renderer's HTTP protocol reports it to whoever drives the renderer —
the C embedding API and the Java binding can hand it to an external player of
their own. The GTK shell itself does not launch an external player; the
`.deb` and `.rpm` packages still `Recommend` one (`mpv`) for opening such
URLs by hand.

Streaming sites (YouTube and friends) drive `<video>` through MSE/`blob:`
with no plain file URL; those play inline through the video helper described
above when libav is present.

## Poster / source metadata (standards, not site scraping)

For pages whose player is JS-driven and has no server-side `<video src>`,
`src/html_lexbor.c` reads **standard** video metadata during HTML parse and
annotates the target element (`data-nd-media-poster` / `data-nd-media-src` /
`data-nd-media-stream`). It reads, in priority order, JSON-LD `VideoObject`
(`contentUrl` / `thumbnailUrl` / `embedUrl`), OpenGraph (`og:video*` /
`og:image*`), and Twitter cards — never a site's private JSON. If a source URL
has an extension the engine decodes it becomes a direct `data-nd-media-src`;
otherwise (e.g. an `og:video` that is a `text/html` embed page, as YouTube
emits) it falls to the MSE stream path — a general rule, not a per-site rule.
The poster comes from `og:image`/`thumbnailUrl`. This covers YouTube, Vimeo and
ordinary `og:video` news articles with the same code. There are no
site-specific media hacks.
