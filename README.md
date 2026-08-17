# Slugged

GPU vector text for OBS Studio, built on [slughorn](https://github.com/AlphaPixel/slughorn) — AlphaPixel's
MIT-licensed implementation of Eric Lengyel's Slug algorithm.

OBS's built-in text sources rasterise glyphs to a bitmap at one fixed size. Scale that source up in a
scene and you magnify the bitmap, so edges go soft and stair-stepped. Slugged keeps glyphs as Bézier
curves on the GPU and solves coverage per pixel, in the fragment shader, at whatever size the pixel
happens to be. Text stays exact at any scale, in the preview and on stream.

## What it adds

Everything the GDI+ text source does — font, colour, opacity, outline, drop shadow, background,
alignment, fixed extents with word wrap, read-from-file, chat log mode — plus:

- **Per-character styling.** Font, size, weight, colour, outline and spacing can vary within one
  source instead of applying to the whole block.
- **Real text shaping.** HarfBuzz for ligatures, kerning and complex scripts, SheenBidi for
  right-to-left and mixed-direction text, and automatic font fallback for characters the chosen font
  lacks.
- **Variable fonts.** Weight, width, slant and any other axis a font exposes, as live sliders.
- **Colour fonts.** COLRv0 and COLRv1 emoji, gradients included.
- **Gradients** across the block or ramped per character.
- **Motion presets** — fade, slide, pop, typewriter, wave — staggered per character, word or line,
  plus continuous scrolling for tickers and credit rolls.
- **Template tokens** like `{time}`, `{uptime}` and your own variables, resolved at render time.
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
