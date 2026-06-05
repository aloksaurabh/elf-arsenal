/* Elf Arsenal — in-flow PS5 temperature gauge driver.

   Looks for a #sl-tempgauge element (only present on the homepage),
   builds the SVG sparkline + readouts inside it, and polls
   /api/fan/temp every 3 s. If the API is unreachable for 3 ticks the
   card is hidden until reads recover. No globals leaked, no deps. */

(function () {
  'use strict';

  if (window.__sonicTempgaugeBooted) return;
  window.__sonicTempgaugeBooted = true;

  var POLL_MS = 3000;
  var FAILURES_BEFORE_HIDE = 3;
  var SAMPLES = 60;
  var W = 240, H = 84, PAD = 4;

  function build(card) {
    if (card.dataset.built === '1') return;
    card.dataset.built = '1';
    card.innerHTML =
      '<div class="sl-tempgauge__left">' +
        '<span class="sl-tempgauge__label">' +
          '<span class="sl-tempgauge__pulse" aria-hidden="true"></span>' +
          'PS5 Temperature' +
          '<button class="sl-t-unit" type="button" ' +
                  'title="Toggle °C / °F (persists per device)" ' +
                  'style="margin-left:8px; font-size:11px; padding:2px 7px; ' +
                         'background:rgba(255,255,255,0.06); color:inherit; ' +
                         'border:1px solid rgba(255,255,255,0.12); ' +
                         'border-radius:5px; cursor:pointer; font-family:inherit;">' +
            '°C' +
          '</button>' +
          '<button class="sl-temp-collapse" type="button" ' +
                  'title="Collapse / expand temperature gauge" ' +
                  'style="margin-left:4px; font-size:11px; padding:2px 6px; ' +
                         'background:rgba(255,255,255,0.04); color:rgba(255,255,255,0.5); ' +
                         'border:1px solid rgba(255,255,255,0.10); ' +
                         'border-radius:5px; cursor:pointer; font-family:inherit; ' +
                         'transition:color .15s, background .15s; line-height:1;">▼</button>' +
        '</span>' +
        '<div class="sl-tempgauge__big">' +
          '<span><span class="sl-t-now">--</span><small class="sl-t-unit-suffix">°C</small></span>' +
          '<span class="sl-tempgauge__delta">—</span>' +
        '</div>' +
        '<span class="sl-tempgauge__sub">hottest of CPU + SoC sensors</span>' +
      '</div>' +
      '<div class="sl-tempgauge__chart">' +
        '<svg viewBox="0 0 ' + W + ' ' + H + '" preserveAspectRatio="none">' +
          '<defs>' +
            '<linearGradient id="sl-tg-fill" x1="0" y1="0" x2="0" y2="1">' +
              '<stop offset="0%"   stop-color="#7ad7ff" stop-opacity="0.55"/>' +
              '<stop offset="55%"  stop-color="#7ad7ff" stop-opacity="0.18"/>' +
              '<stop offset="100%" stop-color="#7ad7ff" stop-opacity="0"/>' +
            '</linearGradient>' +
            '<linearGradient id="sl-tg-line" x1="0" y1="0" x2="1" y2="0">' +
              '<stop offset="0%"   stop-color="#50d890"/>' +
              '<stop offset="55%"  stop-color="#f3c969"/>' +
              '<stop offset="80%"  stop-color="#f3925a"/>' +
              '<stop offset="100%" stop-color="#ee5a5a"/>' +
            '</linearGradient>' +
            '<filter id="sl-tg-glow"><feGaussianBlur stdDeviation="1.6"/></filter>' +
          '</defs>' +
          '<line  class="sl-t-thresh" x1="0" y1="20" x2="' + W + '" y2="20" ' +
                  'stroke="rgba(255,255,255,.85)" stroke-width="1" ' +
                  'stroke-dasharray="3,3" opacity=".7"/>' +
          '<path  class="sl-t-area"      fill="url(#sl-tg-fill)" d=""/>' +
          '<path  class="sl-t-glow-line" fill="none" stroke="url(#sl-tg-line)" ' +
                  'stroke-width="3" stroke-linecap="round" filter="url(#sl-tg-glow)" opacity=".55" d=""/>' +
          '<path  class="sl-t-line-path" fill="none" stroke="url(#sl-tg-line)" ' +
                  'stroke-width="2" stroke-linecap="round" d=""/>' +
          '<circle class="sl-t-head" cx="' + W + '" cy="32" r="4" ' +
                  'fill="#fff" stroke="rgba(0,0,0,.5)" stroke-width="1"/>' +
        '</svg>' +
        '<div class="sl-tempgauge__threshold-pill" style="top:20px;">--°</div>' +
      '</div>' +
      '<div class="sl-tempgauge__pips">' +
        '<div class="pip">' +
          '<div class="pip__val"><span class="sl-t-min">--</span><small>°</small></div>' +
          '<div class="pip__label">min · 60s</div>' +
        '</div>' +
        '<div class="pip">' +
          '<div class="pip__val"><span class="sl-t-avg">--</span><small>°</small></div>' +
          '<div class="pip__label">avg</div>' +
        '</div>' +
        '<div class="pip">' +
          '<div class="pip__val"><span class="sl-t-max">--</span><small>°</small></div>' +
          '<div class="pip__label">max · 60s</div>' +
        '</div>' +
      '</div>' +
      '<div class="sl-drives-hdr" style="grid-column:1/-1; display:flex; align-items:center; ' +
           'margin-top:4px; padding:5px 12px 3px; border-top:1px solid rgba(255,255,255,0.08);">' +
        '<button class="sl-drv-collapse" type="button" ' +
                'title="Collapse / expand storage &amp; drives" ' +
                'style="font-size:11px; padding:2px 8px; ' +
                       'background:rgba(255,255,255,0.04); color:rgba(255,255,255,0.5); ' +
                       'border:1px solid rgba(255,255,255,0.10); ' +
                       'border-radius:5px; cursor:pointer; font-family:inherit; ' +
                       'transition:color .15s, background .15s; line-height:1;">▼ Storage &amp; Drives</button>' +
      '</div>' +
      '<div class="sl-m2gauge" style="display:none; grid-column:1/-1;' +
           ' padding:8px 12px; align-items:center; gap:12px; font-size:12.5px;">' +
        '<span style="color:var(--fg-1,#9aa6c4); min-width:90px;">💾 M.2 NVMe</span>' +
        '<div style="flex:1; height:6px; background:rgba(255,255,255,0.06);' +
             ' border-radius:4px; overflow:hidden; position:relative;">' +
          '<div class="sl-m2-fill" style="height:100%; width:0%;' +
               ' background:linear-gradient(90deg, #50d890, #f3c969, #f3925a, #ee5a5a);' +
               ' transition: width 0.4s ease;"></div>' +
        '</div>' +
        '<span style="font-family:ui-monospace,Menlo,Consolas,monospace;' +
             ' min-width:48px; text-align:right;">' +
          '<span class="sl-m2-now">--</span><span class="sl-m2-unit">°C</span>' +
        '</span>' +
      '</div>' +
      '<div class="sl-drives-container" style="grid-column:1/-1; display:contents;"></div>';
  }

  /* Per-CONSOLE unit preference (server-side). Stored in elf-arsenal's
     /data/elf-arsenal/config.ini as `temp_unit=C|F`, served via
     /api/state.tempUnit, set via /api/state?tempUnit=F. Survives browser
     swap and IP changes. localStorage kept as instant-read mirror so the
     UI doesn't flash the wrong unit on first paint. */
  function getUnit() {
    try { return localStorage.getItem('sl-temp-unit') === 'F' ? 'F' : 'C'; }
    catch (_) { return 'C'; }
  }
  function setUnit(u) {
    var v = (u === 'F' || u === 'f') ? 'F' : 'C';
    try { localStorage.setItem('sl-temp-unit', v); } catch (_) {}
    /* Push to server so the next browser/device picks it up. */
    try { fetch('/api/state?tempUnit=' + v, { cache: 'no-store' }); } catch (_) {}
  }
  /* On boot, sync localStorage from the server's persisted value so
     this browser/device picks up the unit set on any other client. */
  (function syncFromServer() {
    fetch('/api/state', { cache: 'no-store' })
      .then(function (r) { return r.ok ? r.json() : null; })
      .then(function (j) {
        if (!j || !j.tempUnit) return;
        var srv = (j.tempUnit === 'F') ? 'F' : 'C';
        try { localStorage.setItem('sl-temp-unit', srv); } catch (_) {}
        /* Bar redraws every ~3s via /api/fan/temp tick, so it'll pick
           up the new unit on its next render without manual repaint. */
      })
      .catch(function () {});
  })();
  function toUnit(c) {
    if (!Number.isFinite(c)) return c;
    return getUnit() === 'F' ? (c * 9 / 5 + 32) : c;
  }
  function unitLabel() { return getUnit() === 'F' ? '°F' : '°C'; }

  function tempY(t, lo, hi) {
    var x = (t - lo) / (hi - lo);
    if (x < 0) x = 0; if (x > 1) x = 1;
    return H - PAD - x * (H - PAD * 2);
  }
  function classify(t, warmC, hotC) {
    if (t >= hotC)  return 'hot';
    if (t >= warmC) return 'warm';
    return 'cool';
  }
  function fmt(v) {
    return Number.isFinite(v) ? v.toFixed(0) : '--';
  }

  function start() {
    var card = document.getElementById('sl-tempgauge');
    if (!card) return;  // homepage only — every other page omits the container.
    build(card);

    /* ── Collapse toggles (temp section + drives section) ── */
    var tempColBtn   = card.querySelector('.sl-temp-collapse');
    var drvColBtn    = card.querySelector('.sl-drv-collapse');
    /* Hide only the numeric content inside __left, not __left itself —
       keeps the label + collapse button always visible. */
    var tempEls      = [
      card.querySelector('.sl-tempgauge__big'),
      card.querySelector('.sl-tempgauge__sub'),
      card.querySelector('.sl-tempgauge__chart'),
      card.querySelector('.sl-tempgauge__pips')
    ];
    var drvContainer = card.querySelector('.sl-drives-container');
    var m2El         = card.querySelector('.sl-m2gauge');

    var tempCol = localStorage.getItem('ea-temp-col') === '1';
    var drvCol  = localStorage.getItem('ea-drv-col')  === '1';

    function applyTempCol(col) {
      tempEls.forEach(function(el) { if (el) el.style.display = col ? 'none' : ''; });
      if (tempColBtn) tempColBtn.textContent = col ? '▶' : '▼';
    }
    function applyDrvCol(col) {
      if (drvContainer) drvContainer.style.display = col ? 'none' : 'contents';
      /* M.2 NVMe is grouped under Storage & Drives — hide it too, but only
         if it was already visible (don't override tempbar's own hide logic). */
      if (m2El && col) m2El.style.display = 'none';
      if (m2El && !col) m2El.style.display = '';  // let draw() decide
      if (drvColBtn) drvColBtn.textContent = (col ? '▶' : '▼') + ' Storage & Drives';
    }
    applyTempCol(tempCol);
    applyDrvCol(drvCol);
    if (tempColBtn) tempColBtn.addEventListener('click', function() {
      tempCol = !tempCol;
      localStorage.setItem('ea-temp-col', tempCol ? '1' : '0');
      applyTempCol(tempCol);
    });
    if (drvColBtn) drvColBtn.addEventListener('click', function() {
      drvCol = !drvCol;
      localStorage.setItem('ea-drv-col', drvCol ? '1' : '0');
      applyDrvCol(drvCol);
    });
    /* ────────────────────────────────────────────────────── */

    var areaPath = card.querySelector('.sl-t-area');
    var linePath = card.querySelector('.sl-t-line-path');
    var glowPath = card.querySelector('.sl-t-glow-line');
    var headDot  = card.querySelector('.sl-t-head');
    var threshLn = card.querySelector('.sl-t-thresh');
    var threshPl = card.querySelector('.sl-tempgauge__threshold-pill');
    var tNow     = card.querySelector('.sl-t-now');
    var tDelta   = card.querySelector('.sl-tempgauge__delta');
    var tMin     = card.querySelector('.sl-t-min');
    var tAvg     = card.querySelector('.sl-t-avg');
    var tMax     = card.querySelector('.sl-t-max');

    var buf = [];
    var prev = null;
    var failures = 0;

    function draw(j) {
      var lo = Number.isFinite(j.minC)  ? j.minC  : 30;
      var hi = Number.isFinite(j.maxC)  ? j.maxC  : 90;
      var warmC = Number.isFinite(j.warmC) ? j.warmC : 65;
      var hotC  = Number.isFinite(j.hotC)  ? j.hotC  : 80;
      var hot = (Number.isFinite(j.hottestC) ? j.hottestC :
                 Number.isFinite(j.cpuC)     ? j.cpuC     :
                 Number.isFinite(j.socC)     ? j.socC     : NaN);
      if (!Number.isFinite(hot)) return;

      buf.push(hot);
      while (buf.length > SAMPLES) buf.shift();
      var N = buf.length;

      var d = '';
      for (var i = 0; i < N; i++) {
        var x = (i / (SAMPLES - 1)) * W;
        var y = tempY(buf[i], lo, hi);
        d += (i === 0 ? 'M' : 'L') + x.toFixed(1) + ',' + y.toFixed(1) + ' ';
      }
      var area = d + 'L' + W + ',' + H + ' L0,' + H + ' Z';
      linePath.setAttribute('d', d);
      glowPath.setAttribute('d', d);
      areaPath.setAttribute('d', area);

      var headX = ((N - 1) / (SAMPLES - 1)) * W;
      var headY = tempY(hot, lo, hi);
      headDot.setAttribute('cx', headX.toFixed(1));
      headDot.setAttribute('cy', headY.toFixed(1));

      tNow.textContent = fmt(toUnit(hot));

      if (prev != null) {
        var dt = toUnit(hot) - toUnit(prev);
        var sign = dt > 0 ? '▲' : dt < 0 ? '▼' : '·';
        tDelta.textContent = sign + ' ' + Math.abs(dt).toFixed(1) + '°';
        tDelta.className = 'sl-tempgauge__delta ' +
            (dt > 0.4 ? 'up' : dt < -0.4 ? 'down' : '');
      }
      prev = hot;

      var min = buf[0], max = buf[0], sum = 0;
      for (var k = 0; k < N; k++) {
        if (buf[k] < min) min = buf[k];
        if (buf[k] > max) max = buf[k];
        sum += buf[k];
      }
      tMin.textContent = fmt(toUnit(min));
      tMax.textContent = fmt(toUnit(max));
      tAvg.textContent = fmt(toUnit(sum / N));

      if (Number.isFinite(j.thresholdC) && j.thresholdC >= lo) {
        var ty = tempY(j.thresholdC, lo, hi);
        threshLn.setAttribute('y1', ty);
        threshLn.setAttribute('y2', ty);
        threshPl.style.top = (ty / H * 100) + '%';
        threshPl.textContent = fmt(toUnit(j.thresholdC)) + '°';
        threshPl.style.display = '';
      } else {
        threshPl.style.display = 'none';
      }

      card.classList.remove('is-cool', 'is-warm', 'is-hot');
      card.classList.add('is-' + classify(hot, warmC, hotC));
      card.classList.remove('sl-tempgauge--hidden');

      var m2 = card.querySelector('.sl-m2gauge');
      var m2Now = card.querySelector('.sl-m2-now');
      var m2Fill = card.querySelector('.sl-m2-fill');
      if (m2 && Number.isFinite(j.m2C)) {
        if (!drvCol) m2.style.display = 'flex';  // drives collapsed → stay hidden
        m2Now.textContent = fmt(toUnit(j.m2C));
        var pct = (j.m2C - lo) / (hi - lo) * 100;
        if (pct < 0) pct = 0; if (pct > 100) pct = 100;
        m2Fill.style.width = pct.toFixed(1) + '%';
      } else if (m2) {
        m2.style.display = 'none';
      }
    }

    function fmtSize(bytes) {
      if (bytes >= 1e12) return (bytes / 1e12).toFixed(1) + ' TB';
      if (bytes >= 1e9)  return (bytes / 1e9).toFixed(1) + ' GB';
      if (bytes >= 1e6)  return (bytes / 1e6).toFixed(0) + ' MB';
      return bytes + ' B';
    }

    function storageBar(used, total) {
      if (!total) return '';
      var pct = Math.min(100, (used / total) * 100);
      var col = pct >= 90 ? '#ff5675' : pct >= 75 ? '#ffb84a' : '#6bff9c';
      return '<div style="margin-top:6px;">' +
        '<div style="display:flex;justify-content:space-between;font-size:10px;' +
             'color:var(--fg-1,#9aa6c4);margin-bottom:2px;">' +
          '<span>' + fmtSize(used) + ' used</span>' +
          '<span>' + fmtSize(total - used) + ' free</span>' +
        '</div>' +
        '<div style="height:5px;background:rgba(255,255,255,0.06);border-radius:3px;overflow:hidden;">' +
          '<div style="height:100%;width:' + pct.toFixed(1) + '%;background:' + col + ';transition:width .4s;"></div>' +
        '</div>' +
        '<div style="font-size:10px;color:rgba(255,255,255,0.3);margin-top:2px;text-align:right;">' +
          pct.toFixed(0) + '% of ' + fmtSize(total) +
        '</div>' +
      '</div>';
    }

    function drawDrives(drives, storage) {
      var container = card.querySelector('.sl-drives-container');
      if (!container) return;
      var u = unitLabel();
      var BORDER_STYLE = 'grid-column:1/-1; margin-top:6px; padding:8px 12px;' +
        ' border-top:1px solid rgba(255,255,255,0.08); font-size:12.5px;';
      var LABEL_STYLE = 'color:var(--fg-1,#9aa6c4); white-space:nowrap;';
      var GRAD = 'linear-gradient(90deg,#50d890,#f3c969,#f3925a,#ee5a5a)';

      var html = '';

      /* Internal SSD + M.2 storage rows */
      if (storage && storage.length > 0) {
        storage.forEach(function(s) {
          if (!s.fsTotalBytes) return;
          var icon = s.label === 'M.2 Expansion' ? '💾' : '🖥️';
          html += '<div style="' + BORDER_STYLE + '">' +
            '<div style="display:flex;align-items:center;justify-content:space-between;margin-bottom:4px;">' +
              '<span style="' + LABEL_STYLE + '">' + icon + ' ' + s.label + '</span>' +
            '</div>' +
            storageBar(s.fsUsedBytes, s.fsTotalBytes) +
          '</div>';
        });
      }

      /* USB drives */
      if (drives && drives.length > 0) {
        var usbIndex = 0;
        drives.forEach(function(d) {
          usbIndex++;
          var tempC = (typeof d.tempC === 'number') ? d.tempC : null;
          var dispTemp = tempC !== null ? fmt(toUnit(tempC)) + u : 'N/A';
          var pct = 0;
          if (tempC !== null) {
            pct = (tempC / 80) * 100;
            if (pct < 0) pct = 0; if (pct > 100) pct = 100;
          }
          html += '<div style="' + BORDER_STYLE + '">' +
            '<div style="display:flex;align-items:center;gap:10px;margin-bottom:' + (d.fsTotalBytes ? '4px' : '0') + ';">' +
              '<span style="' + LABEL_STYLE + '">🔌 USB ' + usbIndex + '</span>' +
              '<div style="flex:1;height:5px;background:rgba(255,255,255,0.06);border-radius:3px;overflow:hidden;">' +
                (tempC !== null
                  ? '<div style="height:100%;width:' + pct.toFixed(1) + '%;background:' + GRAD + ';transition:width .4s;"></div>'
                  : '<div style="height:100%;width:0%;"></div>') +
              '</div>' +
              '<span style="font-family:ui-monospace,monospace;font-size:11px;min-width:44px;text-align:right;">' + dispTemp + '</span>' +
            '</div>' +
            (d.fsTotalBytes ? storageBar(d.fsUsedBytes, d.fsTotalBytes) : '') +
          '</div>';
        });
      }

      container.innerHTML = html;
    }

    /* Unit toggle button + suffix labels. The redraw triggers via cached
       last-seen j; safest path is to immediately re-render labels and let
       the next tick (≤3s) refresh the numeric values. */
    function paintUnitLabels() {
      var u = unitLabel();
      var btn = card.querySelector('.sl-t-unit');
      if (btn) btn.textContent = u;
      var suf = card.querySelector('.sl-t-unit-suffix');
      if (suf) suf.textContent = u;
      var m2u = card.querySelector('.sl-m2-unit');
      if (m2u) m2u.textContent = u;
    }
    paintUnitLabels();
    var unitBtn = card.querySelector('.sl-t-unit');
    if (unitBtn) {
      unitBtn.addEventListener('click', function () {
        setUnit(getUnit() === 'F' ? 'C' : 'F');
        paintUnitLabels();
        tick();
      });
    }

    var lastDrives = [];
    var lastStorage = [];
    async function tick() {
      try {
        var r = await fetch('/api/fan/temp', { cache: 'no-store' });
        if (!r.ok) throw new Error('http ' + r.status);
        var j = await r.json();
        failures = 0;
        draw(j);
      } catch (e) {
        failures++;
        if (failures >= FAILURES_BEFORE_HIDE) {
          card.classList.add('sl-tempgauge--hidden');
        }
      }
      try {
        var dr = await fetch('/api/sensors/drives', { cache: 'no-store' });
        if (dr.ok) {
          var dj = await dr.json();
          if (dj && Array.isArray(dj.drives)) {
            lastDrives = dj.drives.filter(function(d) { return !d.accessDenied; });
          }
          if (dj && Array.isArray(dj.storage)) {
            lastStorage = dj.storage;
          }
        }
      } catch (_) {}
      drawDrives(lastDrives, lastStorage);
    }

    tick();
    setInterval(tick, POLL_MS);
    document.addEventListener('visibilitychange', function () {
      if (document.visibilityState === 'visible') tick();
    });
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', start);
  } else {
    start();
  }
})();
