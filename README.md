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
- **Glow and soft shadows.** A glow that is solid at the letters' edge and fades out, and drop
  shadows with a soft edge, both blurred on the GPU at any size.
- **Gradients**, linear or radial, with any number of colours between the ends (and presets),
  evaluated per pixel across the whole text or across each
  character.
- **Text filled with another source** — a video, a game capture, a scene — showing through the
  letters. The overlay filter can also show its source only inside the letters, or cut the letters
  out of it.
- **Rounded backgrounds**, and outlines and shadows that stay correct under fades and translucency.
- **Motion presets** — fade, slide, pop, typewriter, wave, blur, spin and bounce — staggered per
  character, word or line. On a text change they can animate only what changed (a counter going
  99 → 100 animates its new digits) and play the old text out first.
- **Effects that keep going** — wave, pulse, shake, glitch, rainbow and shimmer — on the whole text
  or on one word, from the editor or with markup like `[shake]`.
- **Numbers that count up** to their new value instead of jumping, and **counters** stepped by
  hotkeys for deaths, wins or goals.
- **Lines that take turns**: sponsors, socials or rules, one line at a time, each change playing
  the motion.
- **Scrolling** tickers and credit rolls, clipped to the source's box.
- **Text that fits its box**: shrink it to fit, and cut off whatever still does not fit with an
  ellipsis.
- **Template tokens** with filters, like `{followers|number}` or `{timer|mm:ss}`: `{time}`,
  `{stream_time}`, a controllable `{timer}`, `{countdown:5:00}`,
  `{until:20:00}` and your own variables.
- **An editor you type in.** Its preview is a live OBS render of the source, and it is also where
  the caret is: click the text and type. Starting points, saved styles and a list of live values
  come with it.
- **An overlay filter** variant, to lay text over any other source.
- **obs-websocket requests** to set variables, step counters, play motion and move to the next line,
  for Streamer.bot, Touch Portal and other remote controls.
- **One-step conversion** of existing GDI+ and FreeType text sources, in place.

## The editor

**Open editor…** in a source's properties opens it. Everything applies as you make it and can be
undone from OBS's **Edit → Undo** (or the editor's own buttons, which are the same history).

- **Starting points.** A new source's editor opens on twelve of them — plain text, name and title,
  clock, countdown, stream timer, ticker, credits roll, message from a bot, text from a file,
  counter, rotating messages and a neon sign. Each
  sets up the text, the look and the motion, and all of it can be changed afterwards. *Choose a
  different starting point…* under **More** brings them back.
- **Type on the preview.** Click the text to put the caret there; select by dragging or
  double-clicking. A live value such as a countdown shows what it currently says, with a dotted
  underline, and behaves as one character: the caret steps over it and Backspace removes it whole.
- **The panel says what it will change.** Its header reads *The whole text* or *Only the selected
  text*. With a selection, a bar above the preview offers size, bold, italic, colour and *Clear
  style*, and settings that differ from the rest of the text are outlined.
- **One panel, folded.** Text, Effects, Layout, Motion and More are sections of one column; a folded
  section shows what it is set to. Outline, shadow, background and scrolling are switches that show
  their settings only while on.
- **Insert live value** lists every token with what it says right now, and holds your own values —
  for this source or shared by every Slugged source. Typing `{` in the text opens the same list.
- **Styles** saves the current look under a name, with or without its motion, and applies a saved
  one to the whole text or to the selection. Saved styles are stored with the plugin, so they are
  there in every scene collection, and also appear in the properties dialog.
- **Motion** is a grid of presets (hover one to try it on your text), when to play it, what moves
  together, and one speed slider; the exact timings are under *Fine-tune*.

The text is typed in a plain box under the preview instead when typing on it cannot work: with
markup on, when the text scrolls, when it comes from a file, and where OBS cannot draw a preview
into the window (Wayland). **Text box** shows that box at any other time too.

With the editor available, the properties dialog keeps only what is quick to change there: the
text, the font, the colour, a saved style, and the timer buttons when the text uses the timer. A
build without Qt has no editor, and its dialog shows every setting.

## Styling model

A source has a **base style** and text can override any part of it per character. Changing the base
reaches every character except the ones styled differently, the way CSS cascades.

In the editor:

- **With text selected**, a control changes just the selection.
- **With nothing selected**, a control changes the base style, and the whole text takes it.
- Each control changes only its own setting: changing the colour never resets a word you made
  bigger. **Match the rest of the text** returns a selection to the base style.
- Paragraph settings (alignment, line height) apply to the paragraphs the selection touches, or to
  all of them.

Scripts, obs-websocket and tools like Streamer.bot drive a Slugged source exactly like a GDI+ one:
the plain `text` property is always the text. When something sets it, Slugged changes only the part
that differs, so the styling of everything else stays put — set `Score: 10` to `Score: 11` and a
styled `Score:` keeps its style.

### Markup

Turn on **Inline markup** (under **More** in the editor) and the text itself carries styling, for when a bot or script sets it:

```
[b]New follower:[/b] [color=#ffcc00]{follower}[/color]
```

Tags: `[b]`, `[i]`, `[u]`, `[s]`, `[color=#rgb|#rrggbb|#rrggbbaa|name]`, `[size=48]`, `[font=Inter]`,
`[weight=600]`, `[outline=#000]`, `[glow]` or `[glow=#ff0]`, `[gradient=#f00,#ff0,#0f0]` (left to
right through every colour listed), and the effects `[wave]`, `[pulse]`, `[shake]`, `[glitch]`,
`[rainbow]` and `[shimmer]` — `[shake=2]` is twice as strong, and `[effect=shake]` works too.
`[/]` closes the most recent tag, `[[` is a literal bracket, and anything unrecognised stays
visible as typed.

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

**Filters.** A bar after a token's name reshapes its value, and filters chain left to right:
`{followers|number}` → `1,234,567`, `{followers|compact}` → `1.2M`, `{name|trim|upper}`,
`{score|pad:3}` → `007`, `{timer|mm:ss}` → `02:05`, `{song|default:Nothing playing}`.

| Filter | Does |
|---|---|
| `upper`, `lower`, `title`, `trim` | Changes case, or trims spaces |
| `number`, `number:.` | Thousands separators (a comma unless one is given) |
| `compact` | `1.2K`, `45.3K`, `120K`, `3.4M` |
| `round:N`, `fixed:N` | N decimals |
| `pad:N` | Zero-pads a number (or space-pads text) to N characters |
| `add:N`, `sub:N`, `mul:N`, `div:N` | Arithmetic |
| `truncate:N` | At most N characters, ending in `…` |
| `plural:one,many` | `{count} {count\|plural:life,lives}` |
| `mm:ss`, `m:ss`, `h:mm:ss`, `hh:mm:ss`, `seconds`, `minutes` | A value in seconds, or a clock, as a clock |
| `default:text` | What shows while the value is missing or empty |

A filter on a live value (`{timer|mm:ss}`) keeps it live.

**The timer.** Every source has a stopwatch behind `{timer}` and `{countdown:…}`. It starts running
when the source is created; start, pause and reset it from under the editor's preview, the
properties dialog, a hotkey, or the `timer_start`, `timer_pause`, `timer_toggle` and `timer_reset`
procs.

**Variables from a script or bot.** Every Slugged source has `set_variable`, `get_variable`,
`clear_variables`, `increment_variable`, `decrement_variable`, `reset_variable`, `replay_motion` and
`next_message` procs, and OBS's own proc handler gains `slugged_set_variable`,
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

**Counters.** Each source names a counter variable (`count` unless changed, under **More** in the
editor) and a step. The *counter +1*, *counter −1* and *reset counter* hotkeys, the buttons next to
it, and the `increment_variable` / `decrement_variable` / `reset_variable` procs (which also take a
`name` and an `amount`) change it where it lives: a value a script set stays a script value, a
shared one stays shared, and anything else is kept in the source's variables list, so it survives a
restart. The *Counter* starting point sets one up as `Deaths: {count}`.

### obs-websocket

With obs-websocket installed, Slugged registers the vendor `slugged`. Each request names a Slugged
source with `sourceName` (and an overlay filter on it with `filterName`); the variable requests
without one act on the shared table.

```json
{"requestType": "CallVendorRequest", "requestData": {
  "vendorName": "slugged", "requestType": "IncrementVariable",
  "requestData": {"sourceName": "Deaths", "name": "count", "amount": 1}}}
```

| Request | Takes |
|---|---|
| `SetVariable` | `name`, `value` |
| `GetVariable` | `name`; returns `value` |
| `IncrementVariable`, `DecrementVariable` | `name` (the counter when omitted), `amount` (the step) |
| `ResetVariable`, `ClearVariables` | `name` / nothing |
| `PlayMotion`, `NextMessage` | — |
| `TimerStart`, `TimerPause`, `TimerToggle`, `TimerReset` | — |

Every response carries `ok`, and `error` when it is false.

## Motion

Every preset but Wave settles into its final state and holds it, so what matters as much as the
preset is when it plays. **Play** decides:

- **When the text changes** — its content, that is. A clock ticking over is not a change; a variable
  being set is. Turn on **Also when a clock or timer ticks** to count those too, so a countdown's
  digits animate as they turn. Choose whether to animate only the characters that changed or all
  of them, and whether to animate the old text out first.
- **When the source is shown** — anywhere, including the Studio Mode preview.
- **When the source goes live** — on the program output. The right choice for intros in Studio Mode.
- **On a loop**, or **only when the settings change**.

There is also a **Play motion** button under the editor's preview, and a hotkey.

The **Amount** control, under *Fine-tune*, is labelled and ranged for the preset it belongs to — a
travel distance for slide and bounce, a starting scale for pop, an angle for spin, a blur width for
blur, a height for wave.

**Count numbers up** makes a number that changes — a follower count set by a bot, a counter, a
score — count through the values in between over a second or so instead of jumping. It keeps the
number's formatting (`1,250`, `3.50`), and only real changes count; a clock ticking over does not.

### Effects

An effect never settles: it plays for as long as the text is on screen. Unlike the motion presets
it is part of the style, so it can be on the whole text or on one selected word (**Effects** in the
editor, or markup such as `[shake]…[/shake]`), and a word can shake while the line it is in
pops in.

- **Wave** bobs each letter up and down a little behind the last.
- **Pulse** makes letters breathe in and out.
- **Shake** jitters them, for anger or impact.
- **Glitch** throws letters sideways now and then, with a red or cyan flash.
- **Rainbow** walks the letters' colour round the hue wheel.
- **Shimmer** sweeps a band of light across the text, then pauses.

**Strength** is relative (1 is each effect's natural amount; distances scale with the font size),
**Speed** is in cycles per second, and **Offset between letters** is how far apart neighbouring
letters are in the cycle — 0 moves them all together.

### Lines that take turns

**Show one line at a time** (under **Motion**) treats every line of the text as a message of its
own and shows them in turn, every few seconds or in random order; empty lines are skipped. Each
change plays the motion on the whole new line, old line out first when that is on, whatever
**Play** says. A *next line* hotkey, button and `next_message` proc move on early. While the editor
is open, every line shows so they can all be edited. The *Rotating messages* starting point sets
one up.

## Scrolling

**Scroll like a ticker** (under **Motion**) moves the text through the source's box: negative
horizontal speeds move left, so text enters from the right edge. With **Repeat** on it loops
seamlessly with a gap; off, it crosses once, and again whenever the source is shown or the motion is
played by the button, hotkey or proc. A horizontal ticker never wraps, and text outside the box is
clipped. An auto-sized source scrolls through its own bounds; choose **Fixed box** under **Layout**
for a ticker bar of a set width.

## Alignment and size

An auto-sized source grows to fit its text, outline, shadow and any italic overhang, so nothing is
cut off. Alignment positions lines within the text block: on a single line of an auto-sized source
there is nothing to align within, so choose **Fixed box** (and give it a height for vertical
alignment). The overlay filter lays out against the source it sits on, so it always has a box.
**Start** and **End** follow the text's direction: Start is the right edge for Arabic or Hebrew.

A fixed box can deal with text too big for it — a long username, a song title. **Shrink to fit**
makes the whole text smaller until it fits, down to a smallest size you choose, and **Cut off
with …** drops whatever still does not fit and ends the text in an ellipsis.

## Glow, shadows and fills

**Glow** draws a soft light around the letters in its own colour, solid at their edge and gone
*size* pixels out, and can be on together with a drop shadow. A drop shadow's **Softness** blurs
its edge over that many pixels. Both are blurred on the GPU from the letters' outlines, so they
stay smooth at any scale.

A gradient runs between its two end colours through any number of **Colours between**, written as
`#ff0080 #ffcc00@0.7` (a position from 0 to 1 after `@` is optional; colours without one are spread
evenly). **Presets** fill in a few ready-made ones.

**Source** fills the letters with another OBS source's picture — a video, a game capture, a
browser source, a whole scene — scaled to cover the text. The source keeps playing while it shows
through the letters, even when no scene shows it. Like any fill it can be the whole text's or one
word's.

The overlay filter has three ways to meet the picture it sits on (**Text and picture**, under
**More**): text over the picture, the picture only inside the letters, or the letters cut out of
the picture.

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
  → EditMap          where the text sits, for typing     text/edit_map.cpp
                     on the preview (editor open only)
  → ShapeCache       outlines, strokes, COLR, bitmaps    render/shape_cache.cpp
  → AtlasCache       slughorn atlas per source           render/atlas_cache.cpp
  → Geometry         layered quads with knock-outs       render/geometry.cpp
  ── graphics thread ─────────────────────────────────────
  → Renderer         upload, draw, scroll, exit motion   render/renderer.cpp
```

`core/`, `text/` and `render/` other than the renderer contain no libobs or Qt code.

Details worth knowing if you work on the renderer, each commented where it matters:

- libobs exposes **no UINT texture format**, so slughorn's `RG16UI` band texture is uploaded as
  `GS_RG16` (UNORM) and multiplied back by 65535 in the shader. `tests/test_shader.cpp` runs the
  shader's fetch logic against a real atlas, so a slughorn packing change the shader does not
  follow fails a test.
- OBS's `.effect` dialect has no `asuint()`, so Slug's sign-bit root classification is expressed as
  boolean logic over three comparisons; the derivation is in `data/effects/slugged.effect`.
- libobs's `struct vec3` is **16 bytes**, so vertex positions are emitted with a stride of four
  floats, and the vertex shader forces `w` to 1.
- Every screen-space derivative is taken once at the top of the pixel shader, outside any branch.
- libobs's OpenGL backend reads **one digit** after `TEXCOORD`, so `TEXCOORD10` would silently read
  `TEXCOORD1`'s data. Vertex attributes stop at `TEXCOORD9` (a `static_assert` guards it), and
  what every quad shares — the motion preset and its timing — is a uniform instead.
- A soft shadow or glow is the coverage averaged over a disk of taps. Widening Slug's one-pixel
  coverage ramp (which the blur motion does) only blurs along the ray the solve weights, so on its
  own it turns a thin stroke into a flat, hard-edged band.

## Licence

GPL-2.0-or-later, matching OBS Studio. slughorn, HarfBuzz and SheenBidi are MIT-licensed.
