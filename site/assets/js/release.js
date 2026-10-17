/* MyCam site — keep the download button honest.
 *
 * The page ships with the current release baked into the HTML, so it is
 * correct with JavaScript off and correct if GitHub is unreachable. This
 * script then asks the API for the latest release and updates the version,
 * size, filename, SHA-256 and href in place.
 *
 * Why not just link to a fixed name: MyCam-Setup-latest.exe does not exist
 * on the releases yet (docs/mycam-website.md §10 — saif's call). Resolving at
 * runtime means the site is right either way, and starts using the fixed name
 * automatically the day it appears.
 */
(function () {
  'use strict';

  var REPO = 'whoissaaif/mycam';
  var API = 'https://api.github.com/repos/' + REPO + '/releases/latest';
  var FIXED_NAME = 'MyCam-Setup-latest.exe';

  function mb(bytes) {
    return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
  }

  function setAll(selector, fn) {
    var nodes = document.querySelectorAll(selector);
    for (var i = 0; i < nodes.length; i++) fn(nodes[i]);
  }

  function pickInstaller(assets) {
    var fixed = null;
    var versioned = null;

    for (var i = 0; i < assets.length; i++) {
      var a = assets[i];
      if (!/\.exe$/i.test(a.name)) continue;
      if (a.name.toLowerCase() === FIXED_NAME.toLowerCase()) fixed = a;
      else if (/^MyCam-Setup-/i.test(a.name)) versioned = a;
    }
    // Prefer the stable name once it exists; fall back to the versioned one.
    return fixed || versioned;
  }

  function apply(release) {
    var installer = pickInstaller(release.assets || []);
    if (!installer) return;

    var tag = release.tag_name || '';

    setAll('.js-version', function (el) { el.textContent = tag; });
    setAll('.js-size', function (el) { el.textContent = mb(installer.size); });
    setAll('.js-filename', function (el) { el.textContent = installer.name; });

    setAll('.js-download', function (el) {
      el.setAttribute('href', installer.browser_download_url);
    });

    // digest arrives as "sha256:<hex>" when GitHub has computed one.
    if (installer.digest && installer.digest.indexOf('sha256:') === 0) {
      var hex = installer.digest.slice(7);
      setAll('.js-sha', function (el) { el.textContent = hex; });
    }

    // Any command line showing the filename should match the real asset.
    setAll('.js-hashcmd', function (el) {
      el.textContent = 'Get-FileHash .\\' + installer.name + ' -Algorithm SHA256';
    });

    document.documentElement.dataset.release = tag;
  }

  // A failure here is not worth a console error in a visitor's browser: the
  // baked-in values are already correct enough to download.
  try {
    fetch(API, { headers: { Accept: 'application/vnd.github+json' } })
      .then(function (r) { return r.ok ? r.json() : null; })
      .then(function (data) { if (data) apply(data); })
      .catch(function () { /* offline, rate-limited, or blocked — fine */ });
  } catch (e) { /* no fetch — fine */ }
})();
