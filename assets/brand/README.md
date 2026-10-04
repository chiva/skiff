# Skiff artwork

The SVGs here are the masters. `scripts/dev.sh icons` renders them to the committed PNGs, so a PSP
build needs no SVG renderer. After editing a master, run it and commit the SVG and the PNGs
together.

| Master | Renders to | Used by |
|---|---|---|
| `skiff-icon0.svg` (144×80) | `assets/psp/ICON0.PNG` | XMB icon of the `skiff` EBOOT |
| `skiff-pic1.svg` (480×272) | `assets/psp/PIC1.PNG` | XMB background while the app is selected |
| `skiff-social.svg` (1280×640) | `assets/github/social-preview.png` | GitHub social preview (uploaded by hand in the repository settings) |

`skiff-icon0.svg` is also the README header.

## Palette

| | Hex |
|---|---|
| Navy (sky) | `#0F2A3D` |
| Teal (sea) | `#1F8A8A` |
| Cream (hull, sails, text) | `#F3EBDD` |
| Orange (mainsail) | `#E8833A` |

## Lettering

All text is converted to paths, so the SVGs render the same without the font installed (GitHub
does not load web fonts in README images). The typeface is
[Bricolage Grotesque](https://github.com/google/fonts/tree/main/ofl/bricolagegrotesque)
(SIL Open Font License 1.1). Artwork made from it carries no licence obligation; the font files
themselves are not in this repository.

| Text | Size | Weight | Optical size | Tracking |
|---|---|---|---|---|
| Wordmark "skiff" | 36 | 800 | 36 | −1 |
| Social preview tagline | 44 | 600 | 44 | 0 |

Text was shaped with HarfBuzz with ligatures off (as browsers do when letter-spacing is set).
Changing the text means outlining it again with these settings.

## Design rules

- ICON0 must read at 1× on the PSP's 480×272 screen: flat colours, thick shapes, no gradients or
  hairlines.
- No Sony, PlayStation or PSP marks, no RomM logo, no controller-button glyphs.
