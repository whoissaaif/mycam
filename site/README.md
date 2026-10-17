# MyCam website

The public download site. Built to [`docs/mycam-website.md`](../docs/mycam-website.md) — that brief is the
spec, and the CSS comments reference its section numbers.

Plain HTML, CSS and JavaScript. **No build step**: what is in this folder is what gets served.

```
site/
  index.html          Home — the nine sections in the brief's order
  get-started.html    Install steps for PC, Android (USB and Wi-Fi) and iPhone, plus troubleshooting
  privacy.html        Privacy policy + code signing policy (both DRAFT, see below)
  404.html            "No signal"
  assets/css/         tokens → base → home → app-screen
  assets/js/          main.js (behaviour), release.js (keeps the download button current)
  assets/img/         demo video, placeholders, favicon, OG card + its source
```

## Run it locally

```
cd site
python -m http.server 8745      # then open http://127.0.0.1:8745
```

Any static server works. Opening `index.html` as a `file://` URL mostly works but the GitHub API call
for release data will be blocked, so the baked-in version and size are shown instead.

## How the download button stays correct

`index.html` ships with the current release written into the HTML, so the page is right with JavaScript
off. `release.js` then asks `api.github.com` for the latest release and updates the version, size,
filename, SHA-256 and link in place.

It prefers an asset named `MyCam-Setup-latest.exe` and falls back to the versioned
`MyCam-Setup-X.Y.Z.exe`. That fixed-name asset **does not exist yet** — if saif starts attaching one
(brief section 10), this page picks it up with no change here.

## Deploying

GitHub Pages from a `gh-pages` branch, serving the contents of this folder at the repo root. Not from
`/docs` on `main`: that folder holds internal project markdown and would be published by accident.

```
git subtree push --prefix site origin gh-pages
```

Then set Pages to the `gh-pages` branch, root folder. Everything is relative, so it works under
`whoissaaif.github.io/mycam/` as well as at a custom domain.

## Regenerating the Open Graph card

`assets/img/og.png` is a screenshot of `assets/img/og-source.html` at exactly 1200×630. Edit the source,
serve the site, and capture that page at that size with any headless browser.

## Still to do

Carried over from the brief's open items:

- **Real before/after footage.** `compare-laptop.svg` and `compare-phone.svg` are clearly-labelled
  placeholders. They need a real laptop-webcam still and a real MyCam capture of the same scene.
- **Privacy and signing wording** need the project owner's sign-off; both pages say so on the page.
- **Self-host the fonts and libraries.** The site currently loads Anybody from Google Fonts and GSAP and
  Lenis from CDNs, which exposes visitor IPs to third parties — awkward for a product whose pitch is
  privacy, and the brief asks for self-hosted subsetted fonts anyway.
- **Display typeface.** Epic Pro needs a web license; the free stand-in Anybody is in use.
- **The demo clip** is a light-background explainer, which sits oddly against the dark theme.
- **Code signing** changes the "Not signed yet" section and the SmartScreen steps once it lands.
