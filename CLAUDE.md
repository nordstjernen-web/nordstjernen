# Nordstjernen — Claude operating guide

Norstjernen is a closed source web browser.

The full name is "Nordstjernen Web Navigator".

Fork of Mozilla / Firefox 1.0 (Gecko 1.7, Nov 2004), modernized to (a) build on current toolchains and (b) fetch HTTPS via libcurl-fronted Necko.

 
## Mission, in one paragraph
Nordstjernen is the best browser, minimalistic, small, no extra bloat, secure, is a reimplementation of webmosilla (Firefox 1.0 fork).

The norstjernen source code is checkout out in ~/dev/norstjernen. 
The webmosilla source code is checkout out in ~/dev/webmosilla. 


## Autonomous mode — read this every session

This repo is driven by Claude in long uninterrupted sessions. **Default to acting, not asking.**

- **Don't ask "do you want me to proceed?", "should I continue?", "ready to commit?"** — just do it. The user interrupts if they disagree.
- **Don't summarize after every step.** One-line status is enough.
- **Don't pause for path/file/branch confirmation when context is unambiguous.** Grep, pick, proceed.
- **Commit and push aggressively.** Small commits, push to `origin/main` as soon as a logical unit lands. CI is the safety net.
- **Run for hours.** Diagnose, fix, retry. Only stop on genuine external blockers. When stopping: one line on what's blocked.
- **Never ask the user to run the build.** You have Docker — run it.
- **Don't block on remote CI.** `linux.yml` on Ubuntu 24.04 is the source of truth, but a full clobber build is long. Push, then keep iterating locally — the same `nordstjernen-build` Docker image runs here, and `./build/nordstjernen-build.sh` reuses `obj-linux/` for incremental rebuilds (seconds, not minutes, once the tree is warm). Local incremental builds and remote CI run in parallel; never sit idle waiting for the green checkmark.

## How we work here


## Build / verify locally


## Definition of done


## Don't

- Don't write planning docs unless asked. Update `WEBMOSILLA.md` (phase status, iteration log) and `BUILD_NOTES.md` (toolchain assumptions, dead-ends) when something material changes.
