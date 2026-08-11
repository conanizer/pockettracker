# PocketTracker landing page

This branch is **not part of the app**. It holds the landing page, which GitHub Pages serves from
`gh-pages` at the repository root: <https://conanizer.github.io/pockettracker/>

It is an orphan branch — it shares no history with `main` and never merges into it.

```
index.html    the whole page — markup, CSS and script in one file, no build step
media/        the demo clips, the logo, the favicon and the social card
.nojekyll     serve the files as they are, without running Jekyll over them
```

## Editing

Edit `index.html` directly and push this branch; that republishes the page. No build step, no
framework, no dependencies.

## Updating for a new release

The download buttons link straight to the release assets rather than to the Releases page, so a new
version means editing `index.html` in three places:

1. the four `href`s under `id="download"` — each carries the version twice, e.g.
   `/releases/download/v0.9.4/PocketTracker-0.9.4-windows-x64.zip`
2. the four `<p class="meta">` lines, which name each file and its size
3. the `<p class="subver">` line under the hero button

`/releases/latest/download/` cannot be used instead: the asset filenames carry the version.

## Clips

`media/*.mp4` are silent H.264, muted and looping. **H.264, not HEVC** — Firefox and most browsers on
Linux will not play HEVC, and the failure is a silent black rectangle. To convert a screen recording:

```
ffmpeg -i in.mp4 -an -c:v libx264 -profile:v main -pix_fmt yuv420p -crf 27 -preset slow \
       -movflags +faststart out.mp4
```

The hero clip and the demo clips are 4:3 (1280×960); the two touchscreen clips are portrait.
