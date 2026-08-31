# Dual Tone Bench — installable web app

These files are the whole app. Put this folder on any HTTPS host and Android
Chrome will offer to install it; after that it launches from its own icon,
full screen, with no browser bars, and keeps working with no signal.

    index.html               the app (all CSS and JS inline)
    manifest.webmanifest     name, icons, standalone display
    sw.js                    service worker — offline cache
    icon-*.png               launcher icons

## Publish it on GitHub Pages

From inside this folder:

    git init -b main
    git add .
    git commit -m "Dual Tone Bench"

Create an empty repo on github.com (no README), then:

    git remote add origin https://github.com/<you>/<repo>.git
    git push -u origin main

In the repo: **Settings → Pages → Source: Deploy from a branch → main / (root) → Save**.

A minute later the app is live at `https://<you>.github.io/<repo>/`.

## Install it on your phone

1. Open that URL in Chrome on Android.
2. Tap **Install** in the page header, or Chrome's ⋮ menu → **Add to Home screen / Install app**.
3. Launch it from the home screen icon. It runs offline from then on.

## Changing the app later

Edit `index.html`, bump `CACHE = 'dual-tone-bench-v1'` in `sw.js` to `-v2`, and push.
Without the bump, installed copies keep serving the old cached page.

## Why it has to be hosted

Chrome only installs apps from a secure origin (`https://` or `localhost`), and
service workers won't register from a `file://` page. Opening `index.html`
straight off the phone's storage still works as a page — it just can't install
or cache.
