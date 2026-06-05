/* Elf Arsenal build-version badge.
   Drops a fixed-position element in the bottom-right corner of every
   page showing "Elf Arsenal <version>". The version comes from
   /version's "version" field (baked in at build time via -DEA_VERSION).

   PS5 WebKit-friendly: avoids the script's `defer` attribute being
   ignored by waiting for DOM readiness explicitly, falls back to XHR
   if fetch() is missing, and still attaches a fallback label when the
   /version request fails or 404s. */
(function () {
  function attachBadge() {
    if (!document.body) return false;
    if (document.getElementById('sonic-verbadge')) return true;
    var el = document.createElement('div');
    el.id = 'sonic-verbadge';
    el.title = 'Elf Arsenal build version';
    el.textContent = 'Elf Arsenal';   // visible immediately even if /version fails
    document.body.appendChild(el);

    /* Inline-style fallback in case verbadge.css didn't load
       (e.g. PS5 WebKit cached an older HTML without the link). */
    if (!el.style.position) {
      el.style.cssText =
        'position:fixed;right:8px;bottom:8px;z-index:2147483646;' +
        'font-family:ui-monospace,Menlo,Consolas,monospace;font-size:11px;' +
        'padding:4px 8px;border-radius:6px;background:rgba(0,0,0,0.55);' +
        'color:rgba(255,255,255,0.72);' +
        'border:1px solid rgba(255,255,255,0.08);' +
        'pointer-events:auto;user-select:none;letter-spacing:0.02em;opacity:0.7;';
    }

    function fillFromJson(j) {
      try {
        /* Prefer the build-time VERSION_TAG (`j.tag`) over the
           git-describe EA_VERSION (`j.version`). The git-describe
           string is noisy: it pins to the oldest annotated tag
           (v1.1.0, since later releases use lightweight API-created
           tags) and carries `-dirty` because the elf is rebuilt
           against the about-to-be-committed source. VERSION_TAG is
           the canonical release name. */
        var v = '';
        if (j) v = j.tag || j.version || '';
        if (v.indexOf('sonic-loader-') === 0) {
          v = v.slice('sonic-loader-'.length);
        }
        if (v) el.textContent = 'Elf Arsenal ' + v;
      } catch (_) {}
    }

    /* fetch path */
    if (typeof fetch === 'function') {
      fetch('/version', { cache: 'no-store' })
        .then(function (r) { return r && r.ok ? r.json() : null; })
        .then(fillFromJson)
        .catch(function () {});
      return true;
    }
    /* XHR fallback for older WebKit */
    try {
      var xhr = new XMLHttpRequest();
      xhr.open('GET', '/version', true);
      xhr.onreadystatechange = function () {
        if (xhr.readyState !== 4) return;
        if (xhr.status === 200) {
          try { fillFromJson(JSON.parse(xhr.responseText)); } catch (_) {}
        }
      };
      xhr.send();
    } catch (_) {}
    return true;
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', attachBadge, false);
    /* Belt-and-suspenders for WebKit's defer / DCL quirks. */
    setTimeout(attachBadge, 1500);
  } else {
    attachBadge();
  }
})();
