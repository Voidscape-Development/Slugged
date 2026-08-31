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

- **Per-character styling.** Font, size, weight, colour, outline, underline, strikeout and spacing
  can vary within one source instead of applying to the whole block.
- **Real text shaping.** HarfBuzz for ligatures, kerning and complex scripts, SheenBidi for
  right-to-left and mixed-direction text, and automatic font fallback for characters the chosen font
  lacks.
- **Variable fonts.** Weight, width, slant and any other axis a font exposes, as live sliders.
- **Colour fonts.** COLRv0 and COLRv1 emoji, gradients included.
- **Gradients** across the block or ramped per character.
- **Motion presets** — fade, slide, pop, typewriter, wave, blur, spin and bounce — staggered per
  character, word or line, replayed on whatever cue suits (a text change, the scene coming up, a
  loop, a hotkey), plus continuous scrolling for tickers and credit rolls.
- **Template tokens** like `{time}`, `{uptime}` and your own variables, resolved at render time.
  Variables can be typed in, pushed in by a script or bot, or read from a file.
- **A WYSIWYG editor window** whose preview is a live OBS render of the source itself.
- **An overlay filter** variant, to lay text over any other source.

## Editing

Add a **Slugged Text** source and click **Edit in Slugged Editor…**. The preview on the left is the
real source, rendered through the real shader, so it shows exactly what the scene shows. Select text
to style just that part; with nothing selected, changes apply to the whole source. Every change
applies immediately.

The standard properties dialog stays fully functional for everything that does not need
per-character control, and the plain `text` property is kept in sync — so obs-websocket, Lua and
Python scripts, and tools like Streamer.bot drive a Slugged source exactly as they drive a GDI+ one.

The two surfaces do not fight over the source. A property from the dialog is written into the
document only when it actually changes, and an edit made in the editor is never diffed back through
the dialog's single-style mirror — so styling one word in the editor stays on that word, and a saved
scene collection reopens with its per-character styling intact. **Opacity** in the dialog is a
whole-source multiplier applied in the shader: it fades the fill, outline and drop shadow together,
while the alpha channel in each colour picker still controls that element on its own.

## Tokens and variables

Anything written `{name}` in a source's text is replaced as the source renders. A token with no
value stays on screen exactly as typed, so a mistyped name is visible rather than silently blanking
the text, and `{{` is a literal brace.

Values are resolved in this order, first match winning: the variables on the source, the variables in
the source's watched file, the table shared by every Slugged source, values read out of OBS, and the
clock and calendar built-ins. So a source can shadow a shared value locally, and you can define your
own `{timer}` without fighting the built-in one.

**Setting variables by hand.** The properties dialog has a **Variables** list of `name=value` lines;
the editor's **Variables** tab has the same table, plus the shared one. Shared variables are saved
with the plugin rather than with a scene collection, so the same value is available in all of them.

**Setting variables from a script or bot.** Every Slugged source exposes `set_variable`,
`clear_variables` and `replay_motion` procs, and OBS's own proc handler gains
`slugged_set_variable`, `slugged_get_variable`, `slugged_erase_variable` and
`slugged_clear_variables` for the shared table:

```lua
local cd = obs.calldata_create()
obs.calldata_set_string(cd, "name", "followers")
obs.calldata_set_string(cd, "value", "1234")
obs.proc_handler_call(obs.obs_get_proc_handler(), "slugged_set_variable", cd)
obs.calldata_destroy(cd)
```

Anything that can already drive an OBS source can drive the per-source list without knowing anything
Slugged-specific, because it is a plain array of strings in the source's settings: obs-websocket's
`SetInputSettings` and Streamer.bot both write it directly.

**Setting variables from a file.** Point **Variables file** at a file of `name=value` lines or a flat
JSON object and it is re-read whenever it changes, which is the shape most bot tooling already
writes.

**Built-ins.** `{time}`, `{time12}`, `{seconds}`, `{date}`, `{date_long}`, `{weekday}`, `{month}`,
`{year}`, `{strftime:%H:%M}` for any other format, `{uptime}` and `{timer}` for how long the source
has existed, `{source}`, `{scene}`, `{preview_scene}`, `{stream_time}`, `{record_time}`, `{fps}`,
`{dropped_frames}`, `{dropped_percent}`, `{cpu}` and `{bitrate}`.

Token expansion rewrites each run's text in place, so per-character styling survives it — style one
word of `Live for {uptime}` and it stays styled as the clock ticks. A token does have to sit inside
one run to be recognised, which it will unless you deliberately styled half of it differently.

## Motion

Every preset but Wave settles into a final state and then holds it, so what matters as much as the
preset is when it plays again. **Replay** decides: only when you change the settings, whenever the
text changes, whenever the source becomes visible (which for a source in a scene means each time that
scene comes up), or on a loop. There is also a **Play now** button in both the dialog and the editor,
and an OBS hotkey, for triggering an intro on cue.

The **Amount** control is labelled and ranged for the preset it belongs to — a travel distance in
pixels for slide and bounce, a starting scale for pop, a starting angle for spin, a blur width for
blur, a wave height for wave — and only carries a value over between presets when that value could
plausibly have been meant for both.

Blur is a real distance-field blur rather than a multi-tap one: Slug already solves coverage as an
analytic ramp one pixel wide, and telling the solve there are fewer pixels per em spreads that same
ramp over as many as you ask for, at the cost of one divide.

## Alignment

Alignment positions lines within the text block, which means it needs a block wider or taller than
the line to do anything. An auto-sized source is exactly as wide as its longest line, so horizontal
alignment moves the shorter lines of a multi-line source and has nothing to do on a single line;
vertical alignment needs **Use fixed size** and a height. Justify never stretches a block's last
line, so it needs word wrapping to have a line to stretch. The overlay filter is the exception on
both counts: it lays out against the source it is attached to, so it has a box to align in without
being given one.

## Migrating

The properties dialog has an **Import from text source** list. Pick any existing GDI+ or FreeType2
text source and Slugged reproduces its font, colour, outline, alignment, background, wrapping and
file/chat-log configuration. A GDI+ gradient imports as its primary colour; rebuild it in the editor,
which offers more control than GDI+ did.

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

Qt 6 and the OBS frontend API are used for the editor window. Building with `-DENABLE_QT=OFF`
produces a working source without the editor, configured entirely from the properties dialog.

## How it fits together

```
Document (rich text runs)
  → Shaper      HarfBuzz + SheenBidi + font fallback
  → Layout      line breaking, alignment, positioned glyphs
  → AtlasCache  glyph outlines → slughorn Atlas → curve + band textures
  → Geometry    one quad per glyph fill, outline and shadow
  → Renderer    libobs gs_* buffers, textures and the Slug effect
```

`core/`, `text/` and most of `render/` contain no libobs or Qt code, so the whole pipeline from
document to vertex data can be built and tested without an OBS instance.

Three details of the port are worth knowing if you work on the renderer, and each is commented where
it matters:

- libobs exposes **no UINT texture format**, so slughorn's `RGBA16UI` band texture is uploaded as
  `GS_RGBA16` (UNORM) and multiplied back by 65535 in the shader. The bytes are identical; only the
  interpretation differs.
- OBS's `.effect` dialect has no `asuint()`, so Slug's sign-bit root classification is expressed as
  boolean logic over three comparisons. The derivation is written out in `data/effects/slugged.effect`.
- libobs's `struct vec3` is **16 bytes**, not 12, so vertex positions are emitted with a stride of
  four floats.

## Licence

GPL-2.0-or-later, matching OBS Studio. slughorn, HarfBuzz and SheenBidi are MIT-licensed.
