# Text and layout

[Index](INDEX.md) · [Canonical Russian](../ru/UI-LAYOUT.md)

Application authors own layout: reserve screen margins, translations, long values and
non-overlapping regions. Rendering does not automatically repair layout, shrink fonts or scroll text.
Ordinary `kind: "text"` retains free positioning: `y` is a baseline; `center` aligns around `x`.

An optional bounded block explicitly supplies `overflow`, `w` and `h`:

```json
{"kind":"text","x":12,"y":128,"w":256,"h":84,
 "text":"Status: ${app.status}","overflow":"wrap"}
```

Place this fragment in `components[].ui`, after the required 280×240 background rectangle.
The [UI + service example](../../examples/thermometer-service/project.json) uses it.
Keep the next field below `y + h` and the right edge within the screen.

For a bounded block, `x,y` mean the **top-left corner**; `w,h` are integer pixels in 1..640.
`center: true` centers each line inside the box. The selected font is never changed.

| `overflow` | Behavior |
| --- | --- |
| `clip` | Single line, clipped to the box. |
| `ellipsis` | Single line; replace trailing complete UTF-8 codepoints with `…` when necessary. |
| `wrap` | Break at spaces/explicit `\n`; split long words between codepoints. Lines do not overlap. Add `…` on the last visible line when text remains. |

Font metrics determine line height. A box too short for one complete line displays no text;
neither the font nor box is automatically adjusted. Test the actual font and translations.
Canvas clipping protects screen boundaries but does not make a layout correct.

Boxes add no scene operations, dynamic buffers or periodic wakeups. Existing text/scene
limits remain. Authors should provide a separate details view, explicit expansion or scrolling
when users need the full content; truncated text does not become readable automatically.

ContractRevision `0x20261115` extends text operation flags without changing operation size
or nominal package/API/Companion versions. Rebuild packages, factory and both consumers;
old incompatible packages/peers are rejected. Do not erase private storage or grants.
Check: `ctest --test-dir build/docs-sdk -C Debug -R text_layout --output-on-failure`.

The complete [text-layout example](../../examples/text-layout/project.json) demonstrates
wrapping, ellipsis, long words and ordinary baseline text. Its margins were checked on
the current rounded Spark LCD; validate the geometry again for other panels.
