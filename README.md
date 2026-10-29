# Slugged

GPU vector text for OBS Studio, built on [slughorn](https://github.com/AlphaPixel/slughorn) — AlphaPixel's
MIT-licensed implementation of Eric Lengyel's Slug algorithm.

OBS's built-in text sources rasterise glyphs to a bitmap at one fixed size. Scale that source up in a
scene and you magnify the bitmap, so edges go soft and stair-stepped. Slugged keeps glyphs as Bézier
curves on the GPU and solves coverage per pixel, in the fragment shader, at whatever size the pixel
happens to be. Text stays exact at any scale, in the preview and on stream.

## What it adds

Everything the GDI+ text source does — font, colour, opacity, outline, drop shadow, background,
alignment, underline and strikeout, fixed extents with word wrap, read-from-file, chat log mode —
plus:

- **Per-character styling** that survives everything else. Style one word in the editor and it
  stays styled when you change the font in the properties dialog, when a script rewrites the text,
  or when a token updates.
- **Inline markup** for scripts and bots: `[b]`, `[i]`, `[color=#f80]`, `[size=48]` and friends.
- **Real text shaping.** HarfBuzz for ligatures, kerning and complex scripts, SheenBidi for
  right-to-left and mixed-direction text (wrapped correctly, line by line), and line breaking that
  works for Chinese, Japanese and Korean.
- **Colour emoji** from COLR fonts (v0 and v1) and from bitmap emoji fonts (CBDT, sbix).
- **Variable fonts.** The weight setting drives a variable font's weight axis; every other axis is a
  slider in the editor.
- **Gradients**, linear or radial, evaluated per pixel across the whole text or across each
  character.
- **Rounded backgrounds**, and outlines and shadows that stay correct under fades and translucency.
- **Motion presets** — fade, slide, pop, typewriter, wave, blur, spin and bounce — staggered per
  character, word or line. On a text change they can animate only what changed (a counter going
  99 → 100 animates its new digits) and play the old text out first.
- **Scrolling** tickers and credit rolls, clipped to the source's box.
- **Template tokens** like `{time}`, `{stream_time}`, a controllable `{timer}`, `{countdown:5:00}`,
  `{until:20:00}` and your own variables.
- **A WYSIWYG editor** whose preview is a live OBS render of the source itself, with undo.
- **An overlay filter** variant, to lay text over any other source.
- **One-step conversion** of existing GDI+ and FreeType text sources, in place.

## Styling model

A source has a **base style** — what the properties dialog shows — and text can override any part
of it per character. Changing the base reaches every character except the ones styled differently,
the way CSS cascades.

In the editor:

- **With text selected**, a control changes just the selection.
- **With nothing selected**, a control changes the base style, and the whole text takes it.
- Each control changes only its own setting: changing the colour never resets a word you made
  bigger. **Clear formatting** returns a selection to the base style.
- Paragraph settings (alignment, direction, line height, spacing) apply to the paragraphs the
  selection touches, or to all of them.

The text box shows the styling — weight, slant, underline, colour, outline, relative size — so you
can see what is styled where. Every change applies immediately and can be undone from OBS's
**Edit → Undo**.

Scripts, obs-websocket and tools like Streamer.bot drive a Slugged source exactly like a GDI+ one:
the plain `text` property is always the text. When something sets it, Slugged changes only the part
that differs, so the styling of everything else stays put — set `Score: 10` to `Score: 11` and a
styled `Score:` keeps its style.

### Markup

Turn on **Inline markup** and the text itself carries styling, for when a bot or script sets it:

```
[b]New follower:[/b] [color=#ffcc00]{follower}[/color]
```

Tags: `[b]`, `[i]`, `[u]`, `[s]`, `[color=#rgb|#rrggbb|#rrggbbaa|name]`, `[size=48]`, `[font=Inter]`,
`[weight=600]`, `[outline=#000]`. `[/]` closes the most recent tag, `[[` is a literal bracket, and
anything unrecognised stays visible as typed.

## Tokens and variables

Anything written `{name}` is replaced as the source renders. A token with no value stays on screen
exactly as typed, so a typo is visible, and `{{` is a literal brace.

Values resolve in this order, first match winning: values a script set on the source, the source's
variables list, its watched variables file, the table shared by every Slugged source, values read
out of OBS, and the built-ins.

**Built-ins.** `{time}`, `{time12}`, `{seconds}`, `{date}`, `{date_long}`, `{weekday}`, `{month}`,
`{year}`, `{strftime:%H:%M}`, `{uptime}`, `{timer}`, `{countdown:5:00}` (counts down on the timer),
`{until:20:00}` and `{until:2026-12-31 20:00}`, `{source}`, `{scene}`, `{preview_scene}`,
`{stream_time}`, `{record_time}`, `{fps}`, `{dropped_frames}`, `{dropped_percent}`, `{cpu}`,
`{bitrate}`.

Digits are fixed-width by default, so a clock or counter does not jitter sideways as it changes.

**The timer.** Every source has a stopwatch behind `{timer}` and `{countdown:…}`. It starts running
when the source is created; start, pause and reset it from the properties dialog, the editor, a
hotkey, or the `timer_start`, `timer_pause`, `timer_toggle` and `timer_reset` procs.

**Variables from a script or bot.** Every Slugged source has `set_variable`, `clear_variables` and
`replay_motion` procs, and OBS's own proc handler gains `slugged_set_variable`,
`slugged_get_variable`, `slugged_erase_variable` and `slugged_clear_variables` for the shared table:

```lua
local cd = obs.calldata_create()
obs.calldata_set_string(cd, "name", "followers")
obs.calldata_set_string(cd, "value", "1234")
obs.proc_handler_call(obs.obs_get_proc_handler(), "slugged_set_variable", cd)
obs.calldata_destroy(cd)
```

The per-source **Variables** list is a plain array of `name=value` strings in the source's settings,
so obs-websocket's `SetInputSettings` and Streamer.bot can write it directly. A **Variables file**
of `name=value` lines or a flat JSON object is re-read whenever it changes. Shared variables are
saved with the plugin rather than with a scene collection, a couple of seconds after they change.

## Motion

Every preset but Wave settles into its final state and holds it, so what matters as much as the
preset is when it plays. **Play** decides:

- **When the text changes** — its content, that is. A clock ticking over is not a change; a variable
  being set is. Choose whether to animate only the characters that changed or all of them, and
  whether to animate the old text out first.
- **When the source is shown** — anywhere, including the Studio Mode preview.
- **When the source goes live** — on the program output. The right choice for intros in Studio Mode.
- **On a loop**, or **only when the settings change**.

There is also a **Play now** button and a hotkey.

The **Amount** control is labelled and ranged for the preset it belongs to — a travel distance for
slide and bounce, a starting scale for pop, an angle for spin, a blur width for blur, a height for
wave.

## Scrolling

**Scroll continuously** moves the text through the source's box: negative horizontal speeds move
left, so text enters from the right edge. With **Repeat** on it loops seamlessly with a gap; off, it
crosses once, and again whenever the source is shown or the motion is played by the button, hotkey
or proc. A horizontal ticker never wraps, and text outside the box
is clipped. An auto-sized source scrolls through its own bounds; turn on **Use a fixed size** for a
ticker bar of a set width.

## Alignment and size

An auto-sized source grows to fit its text, outline, shadow and any italic overhang, so nothing is
cut off. Alignment positions lines within the text block: on a single line of an auto-sized source
there is nothing to align within, so turn on **Use a fixed size** (and give it a height for vertical
alignment). The overlay filter lays out against the source it sits on, so it always has a box.
**Start** and **End** follow the text's direction: Start is the right edge for Arabic or Hebrew.

## Converting existing text sources

**Tools → Convert text sources to Slugged…** lists every GDI+ and FreeType 2 text source. The ones
you pick are replaced in every scene that uses them by a Slugged source of the same name, keeping
position, size, crop, visibility, order and filters. Font, colours (gradients included), outline,
background, alignment, wrapping and file or chat-log reading carry over. The originals are kept,
renamed with "(old)", unless you choose to delete them.

## Building

Dependencies are vendored as submodules and built from source, so a clone plus a configure is all
that is needed:

```sh
git clone --recursive https://github.com/Voidscape-Development/Slugged.git
cd Slugged
cmake --preset ubuntu-x86_64     # or windows-x64, macos
cmake --build --preset ubuntu-x86_64
```

| Dependency | Source | Why vendored |
|---|---|---|
| slughorn | submodule | No distribution packages exist |
| HarfBuzz | submodule | Consistent shaping across all three platforms |
| SheenBidi | submodule | Small, CMake-native bidi implementation |
| FreeType | system / obs-deps | Already shipped with OBS |
| fontconfig | system (Linux only) | Font enumeration; Windows uses DirectWrite, macOS CoreText |

Qt 6 and the OBS frontend API are used for the editor and the conversion tool. Building with
`-DENABLE_QT=OFF` produces a working source without them, configured entirely from the properties
dialog.

### Tests

The document model, markup, tokens, text pipeline and CPU-side geometry have no OBS or Qt
dependencies and are covered by a standalone test project:

```sh
cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
```

## How it fits together

```
Settings (schema 2)  → SourceConfig snapshot             obs/settings.cpp
  Document           base style + per-run overrides      core/document.hpp
  → tokens expanded  live vs content values              core/tokens.cpp
  ── rebuild worker thread ───────────────────────────── render/rebuild_worker.cpp
  → Shaper           HarfBuzz + SheenBidi + fallback     text/shaper.cpp
  → Layout           logical line breaking, per-line     text/layout.cpp
                     bidi reordering, alignment
  → ShapeCache       outlines, strokes, COLR, bitmaps    render/shape_cache.cpp
  → AtlasCache       slughorn atlas per source           render/atlas_cache.cpp
  → Geometry         layered quads with knock-outs       render/geometry.cpp
  ── graphics thread ─────────────────────────────────────
  → Renderer         upload, draw, scroll, exit motion   render/renderer.cpp
```

`core/`, `text/` and `render/` other than the renderer contain no libobs or Qt code.

Details worth knowing if you work on the renderer, each commented where it matters:

- libobs exposes **no UINT texture format**, so slughorn's `RGBA16UI` band texture is uploaded as
  `GS_RGBA16` (UNORM) and multiplied back by 65535 in the shader.
- OBS's `.effect` dialect has no `asuint()`, so Slug's sign-bit root classification is expressed as
  boolean logic over three comparisons; the derivation is in `data/effects/slugged.effect`.
- libobs's `struct vec3` is **16 bytes**, so vertex positions are emitted with a stride of four
  floats, and the vertex shader forces `w` to 1.
- Every screen-space derivative is taken once at the top of the pixel shader, outside any branch.

## Licence

GPL-2.0-or-later, matching OBS Studio. slughorn, HarfBuzz and SheenBidi are MIT-licensed.
