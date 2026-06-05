/* Elf Arsenal — UI auto-translation via /api/translate (Google gtx).
 *
 * Behavior:
 *   1. On boot, read 'sl-ui-lang' from localStorage. If unset/'en',
 *      do nothing (UI is authored in English).
 *   2. Otherwise walk the DOM, collect every translatable text node
 *      and translation-eligible attribute (title, placeholder,
 *      aria-label), look each one up in localStorage 'sl-ui-i18n-<lang>'.
 *      Cache miss → POST /api/translate?to=<lang>&q=<text>, stash in
 *      localStorage, write to DOM.
 *   3. After every translation cycle, save the merged map back to
 *      localStorage so subsequent boots are instant.
 *   4. A MutationObserver watches for new nodes (modals, lazy-rendered
 *      sections) and translates them as they appear.
 *
 * What we DO NOT translate (preserve verbatim):
 *   - Anything inside <code>, <pre>, <kbd>, <samp>, <tt>
 *   - Anything with class .addr, .cli, .mono, .verbatim, .nox
 *   - Anything with attribute data-i18n-skip or inside [data-i18n-skip]
 *   - Crypto wallet rows, IP addresses, file paths, FW versions,
 *     PPSA/PPSB/CUSA title IDs (regex-filtered before sending)
 *   - Pure punctuation, digits, single emojis, or strings under 2 chars
 *   - Project nouns: Elf Arsenal, Kstuff, BackPork, etc. (allowlist)
 *
 * Concurrency: limit to 4 in-flight requests so we don't melt
 * translate.googleapis.com. Failures are logged and the original
 * string stays on screen.
 */

(function () {
  'use strict';
  try {
  console.log('[i18n] module loading…');

  const LS_LANG_KEY  = 'sl-ui-lang';
  const LS_CACHE_KEY = (lang) => 'sl-ui-i18n-' + lang;
  /* Server-side already serializes all outbound HTTPS calls behind
     a mutex (Sony's HTTP/SSL stack isn't thread-safe), so client
     concurrency stays at 1. With batching, each request now carries
     up to BATCH_SIZE strings instead of one, so total HTTP rounds
     drop from ~250 to ~6 for a typical full UI translation. */
  const CONCURRENCY  = 1;
  /* Batched-request strings per HTTP call. The server caps at 16 KB of
     `q=` payload and 100 strings per batch; keep us well under both. */
  const BATCH_SIZE   = 40;
  const US_SEP       = '\x1F';   /* ASCII Unit Separator — never in normal text */

  /* Code identifiers / project nouns we never translate.
     NOTE: NO Unicode property escapes (\p{...}) here — older WebKit
     throws "invalid Unicode property escape" on `new RegExp` and
     kills this whole IIFE, which means the picker handler below
     never attaches and the dropdown silently does nothing. */
  const VERBATIM_RX = new RegExp(
    [
      '^\\s*$',
      /* pure numbers / hex / percentages / ports / FW codes */
      '^[0-9A-Fa-fxX:\\.\\,\\-\\s%]+$',
      /* IPv4 + IPv6-ish */
      '^[0-9]{1,3}(\\.[0-9]{1,3}){3}(:\\d+)?$',
      /* file paths */
      '^[\\/~][\\w\\-\\.\\/]*$',
      /* PPSA/PPSB/CUSA title IDs */
      '^(PPSA|PPSB|CUSA)\\d{5}',
      /* domains / urls */
      '^(https?:)?\\/\\/',
      '\\.(com|net|org|dev|io|gg|tv)(\\/|$)',
      /* plain ASCII-punctuation-only / whitespace-only strings */
      '^[\\s!"#$%&\'()*+,\\-./:;<=>?@\\[\\\\\\]^_`{|}~]+$',
    ].join('|')
  );

  const VERBATIM_TERMS = new Set([
    'Elf Arsenal', 'Sonic Loader', 'Kstuff', 'kstuff', 'kstuff-lite',
    'BackPork', 'ShadowMountPlus', 'SMP', 'ftpsrv', 'zftpd',
    'klogsrv', 'nanoDNS', 'CheatRunner', 'Cheat Engine', 'LapyJB', 'Y2JB',
    'LapyJB daemon', 'Lapy JB', 'Lapy JB daemon',
    'PHU', 'PS5', 'PS4', 'PSN', 'Sony', 'GitHub', 'Gitea',
    'FTP', 'DNS', 'DPI', 'PKG', 'SDK', 'SFO', 'ELF', 'JSON', 'JS',
    'M.2', 'NVMe', 'SoC', 'CPU', 'GPU', 'RAM', 'NAND', 'IO',
    'USB', 'SMB', 'JB', 'NP', 'UDS', 'OK', 'ON', 'OFF',
    'BTC', 'USDC', 'SPL', 'Sol', 'ETH',
    'soniciso', 'sonic-iso', 'etaHEN', 'EchoStretch',
    /* App / feature names — leave as-is in any language. */
    'Home', 'Homebrew',
    'FPKG Guard', 'fpkg-guard', 'FPKG',
    'daemon',
    'How Long to Beat', 'HLTB',
    'Black',
    /* NP fake sign-in section */
    'NP Fake Sign-In',
    'NP fake sign-in',
    'Fake Sign-In',
    'fake-signin',
    'np-fake-signin',
    'np-restore-account', 'NP Restore Account',
    /* Garlic ecosystem */
    'Garlic', 'GarlicSaves', 'Garlic SaveMgr', 'Garlic Worker',
    'garlicsaves.com', 'garlic-worker', 'garlic-savemgr',
    /* Other */
    'SaveMgr', 'PKG Zone', 'Klog', 'PS5 Linux',
  ]);

  function shouldTranslate(text) {
    const t = (text || '').trim();
    if (t.length < 2) return false;
    if (VERBATIM_RX.test(t)) return false;
    if (VERBATIM_TERMS.has(t)) return false;
    return true;
  }

  /* Walk up — don't translate inside excluded containers. */
  const SKIP_TAGS = new Set([
    'CODE', 'PRE', 'KBD', 'SAMP', 'TT', 'SCRIPT', 'STYLE',
    'TEXTAREA', 'INPUT',
  ]);
  const SKIP_CLASSES = ['addr', 'cli', 'mono', 'verbatim', 'nox',
                        'tile-name', 'tile-id', 'spotlight-title'];

  function skipContainer(node) {
    let n = node;
    while (n && n !== document.body) {
      if (n.nodeType !== 1) { n = n.parentNode; continue; }
      if (SKIP_TAGS.has(n.tagName)) return true;
      if (n.hasAttribute && n.hasAttribute('data-i18n-skip')) return true;
      if (n.classList) {
        for (const c of SKIP_CLASSES) {
          if (n.classList.contains(c)) return true;
        }
      }
      n = n.parentNode;
    }
    return false;
  }

  function loadCache(lang) {
    try {
      const raw = localStorage.getItem(LS_CACHE_KEY(lang));
      if (!raw) return {};
      const j = JSON.parse(raw);
      return (j && typeof j === 'object') ? j : {};
    } catch (_) { return {}; }
  }

  function saveCache(lang, map) {
    try { localStorage.setItem(LS_CACHE_KEY(lang), JSON.stringify(map)); }
    catch (_) {}
  }

  /* Hydrate the localStorage cache from disk on first run after
     console reboot — disk survives, localStorage doesn't. */
  async function hydrateFromDisk(lang) {
    try {
      const r = await fetch('/api/translate/cache?to=' + encodeURIComponent(lang),
                            { method: 'POST' });
      const j = await r.json();
      if (j && j.ok && j.map && typeof j.map === 'object') {
        const local = loadCache(lang);
        let added = 0;
        for (const k of Object.keys(j.map)) {
          if (!(k in local)) { local[k] = j.map[k]; added++; }
        }
        if (added > 0) saveCache(lang, local);
        return local;
      }
    } catch (_) {}
    return loadCache(lang);
  }

  /* Per-cycle stats reported back to the Language picker UI so we can
     surface real progress instead of a spinner that lies about what
     happened. `onProgress` is fired after each completed batch (or
     each per-string completion inside a batch, depending on flow). */
  let cycleStats = null;

  function reportProgress(text) {
    if (cycleStats && typeof cycleStats.onProgress === 'function') {
      try { cycleStats.onProgress(cycleStats, text); } catch (_) {}
    }
  }

  /* Translate up to BATCH_SIZE strings in one HTTP round-trip via the
     /api/translate/batch endpoint. Returns a {sourceString: translation}
     map for the strings that came back successfully; failures are
     omitted (caller falls back to the original). */
  async function translateBatch(lang, strings) {
    if (!strings.length) return {};
    const q = strings.join(US_SEP);
    try {
      const r = await fetch('/api/translate/batch?to=' + encodeURIComponent(lang)
                           + '&q=' + encodeURIComponent(q),
                           { method: 'POST' });
      const j = await r.json();
      if (!j || !j.ok || !Array.isArray(j.t)) {
        if (cycleStats) {
          cycleStats.failed += strings.length;
          if (!cycleStats.firstErr && j && j.error) cycleStats.firstErr = j.error;
        }
        return {};
      }
      const out = {};
      for (let i = 0; i < strings.length && i < j.t.length; i++) {
        const t = j.t[i];
        if (typeof t === 'string' && t.length > 0) {
          out[strings[i]] = t;
          if (cycleStats) cycleStats.ok++;
        } else if (cycleStats) {
          cycleStats.failed++;
        }
      }
      return out;
    } catch (e) {
      if (cycleStats) {
        cycleStats.failed += strings.length;
        if (!cycleStats.firstErr) cycleStats.firstErr = e.message || 'fetch error';
      }
      return {};
    }
  }

  /* Collect every text node + translatable attribute under root.
     Original strings are preserved on the node via data-i18n-orig
     so language switching can revert without a reload. */
  function collectTargets(root) {
    const textTargets = [];
    const attrTargets = [];

    const walker = document.createTreeWalker(
      root, NodeFilter.SHOW_TEXT, {
        acceptNode(n) {
          if (skipContainer(n.parentNode)) return NodeFilter.FILTER_REJECT;
          if (!shouldTranslate(n.nodeValue)) return NodeFilter.FILTER_REJECT;
          return NodeFilter.FILTER_ACCEPT;
        }
      });
    let n; while ((n = walker.nextNode())) textTargets.push(n);

    const elWalker = document.createTreeWalker(
      root, NodeFilter.SHOW_ELEMENT, {
        acceptNode(el) {
          if (skipContainer(el)) return NodeFilter.FILTER_REJECT;
          return NodeFilter.FILTER_ACCEPT;
        }
      });
    let el;
    while ((el = elWalker.nextNode())) {
      for (const attr of ['title', 'placeholder', 'aria-label']) {
        const v = el.getAttribute && el.getAttribute(attr);
        if (v && shouldTranslate(v)) attrTargets.push({ el, attr, v });
      }
    }
    return { textTargets, attrTargets };
  }

  /* Set to a MutationObserver instance once boot() wires one up. We
     disconnect it during applyLang's DOM-write phase to prevent a
     feedback loop: every translated text node would otherwise trigger
     the observer, which would call applyLang again, which would
     translate more nodes, ad infinitum. Hundreds of concurrent
     translate batches pile up server-side and crash websrv. */
  let g_observer = null;

  async function applyLang(lang, root, onProgress) {
    if (!lang || lang === 'en') { revertAll(root || document.body); return { ok:0, failed:0 }; }
    const cache = await hydrateFromDisk(lang);
    const { textTargets, attrTargets } = collectTargets(root || document.body);

    /* Pre-stash originals so revert + relookup work. */
    for (const node of textTargets) {
      if (node.__i18nOrig == null) node.__i18nOrig = node.nodeValue;
    }
    for (const t of attrTargets) {
      t.el.__i18nAttrOrig = t.el.__i18nAttrOrig || {};
      if (t.el.__i18nAttrOrig[t.attr] == null) t.el.__i18nAttrOrig[t.attr] = t.v;
    }

    /* Build the universe of unique strings, then partition into
       cache-hit vs needs-fetching. */
    const unique = new Set();
    for (const n of textTargets) unique.add((n.__i18nOrig || '').trim());
    for (const a of attrTargets) unique.add(
      ((a.el.__i18nAttrOrig && a.el.__i18nAttrOrig[a.attr]) || a.v || '').trim());
    const allStrings = Array.from(unique).filter(s => s.length > 0);
    const missing = allStrings.filter(s => !(s in cache));
    const cacheHitCount = allStrings.length - missing.length;

    const isTopCall = (cycleStats === null);
    if (isTopCall) {
      cycleStats = {
        unique:    allStrings.length,
        toFetch:   missing.length,
        cacheHits: cacheHitCount,
        ok: 0,
        failed: 0,
        firstErr: null,
        startedAt: Date.now(),
        onProgress: onProgress || null,
      };
      reportProgress(null);
    }

    /* Batch the misses. Each batch is one HTTP call; the server fans
       out into a single Google call carrying all N strings. */
    for (let i = 0; i < missing.length; i += BATCH_SIZE) {
      const slice = missing.slice(i, i + BATCH_SIZE);
      const translated = await translateBatch(lang, slice);
      for (const k of Object.keys(translated)) cache[k] = translated[k];
      saveCache(lang, cache);
      reportProgress(slice[slice.length - 1]);
    }

    /* CRITICAL: pause the MutationObserver before we touch the DOM.
       Each text-node mutation we make would otherwise re-trigger the
       observer, which calls applyLang again, which mutates more
       nodes, … the loop ends only when websrv crashes. */
    if (g_observer) g_observer.disconnect();
    try {
      /* Revert pre-pass: walk EVERY text node with __i18nOrig in the
         tree (not just the ones we collected). Some nodes that were
         translated by a previous cycle may now be in a skip container
         (data-i18n-skip added later) or now match VERBATIM_TERMS we
         added in a newer build. Roll those back to their original so
         users don't have to wipe their cache to undo wrong terms. */
      const allTextWalker = document.createTreeWalker(
        root || document.body, NodeFilter.SHOW_TEXT);
      let revn;
      while ((revn = allTextWalker.nextNode())) {
        if (revn.__i18nOrig == null) continue;
        if (skipContainer(revn.parentNode) ||
            !shouldTranslate(revn.__i18nOrig)) {
          if (revn.nodeValue !== revn.__i18nOrig) revn.nodeValue = revn.__i18nOrig;
        }
      }
      const allElWalker = document.createTreeWalker(
        root || document.body, NodeFilter.SHOW_ELEMENT);
      let revel;
      while ((revel = allElWalker.nextNode())) {
        if (!revel.__i18nAttrOrig) continue;
        const inSkip = skipContainer(revel);
        for (const a of Object.keys(revel.__i18nAttrOrig)) {
          const orig = revel.__i18nAttrOrig[a];
          if (inSkip || !shouldTranslate(orig)) {
            if (revel.getAttribute(a) !== orig) revel.setAttribute(a, orig);
          }
        }
      }

      for (const node of textTargets) {
        const orig = node.__i18nOrig || '';
        const t = cache[orig.trim()];
        if (t == null) continue;
        const lead  = orig.match(/^\s*/)[0];
        const trail = orig.match(/\s*$/)[0];
        node.nodeValue = lead + t + trail;
      }
      for (const a of attrTargets) {
        const orig = (a.el.__i18nAttrOrig && a.el.__i18nAttrOrig[a.attr]) || a.v;
        const t = cache[(orig || '').trim()];
        if (t != null) a.el.setAttribute(a.attr, t);
      }
    } finally {
      if (g_observer) {
        g_observer.observe(document.body, { childList: true, subtree: true });
      }
    }

    if (isTopCall) {
      const stats = cycleStats;
      cycleStats = null;
      return stats;
    }
    return { ok: 0, failed: 0 };
  }

  function revertAll(root) {
    const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
    let n; while ((n = walker.nextNode())) {
      if (n.__i18nOrig != null) {
        n.nodeValue = n.__i18nOrig;
      }
    }
    const elWalker = document.createTreeWalker(root, NodeFilter.SHOW_ELEMENT);
    let el;
    while ((el = elWalker.nextNode())) {
      if (el.__i18nAttrOrig) {
        for (const a of Object.keys(el.__i18nAttrOrig)) {
          el.setAttribute(a, el.__i18nAttrOrig[a]);
        }
      }
    }
  }

  /* Public API */
  window.SLI18n = {
    getLang() {
      try { return localStorage.getItem(LS_LANG_KEY) || 'en'; }
      catch (_) { return 'en'; }
    },
    async setLang(lang, onProgress) {
      try { localStorage.setItem(LS_LANG_KEY, lang); } catch (_) {}
      if (lang === 'en') { revertAll(document.body); return { ok:0, failed:0 }; }
      return await applyLang(lang, document.body, onProgress);
    },
    async clearCache(lang) {
      try { localStorage.removeItem(LS_CACHE_KEY(lang)); } catch (_) {}
      try { await fetch('/api/translate/clear?to=' + encodeURIComponent(lang),
                        { method: 'POST' }); } catch (_) {}
    },
    /* For external callers (e.g. modalView) — translate a freshly
       inserted subtree using the current language. */
    async translateSubtree(root) {
      const lang = this.getLang();
      if (!lang || lang === 'en') return;
      await applyLang(lang, root);
    },
  };

  /* Wire the in-page Language picker. Lives here (not inline in
     launcher.html) because the launcher.html inline <script> runs
     during HTML parse — earlier than this deferred module — so it
     could never see window.SLI18n. */
  function wireLangPicker() {
    const sel    = document.getElementById('ui-lang-select');
    const clear  = document.getElementById('ui-lang-clear-cache');
    const status = document.getElementById('ui-lang-status');
    const progWrap = document.getElementById('ui-lang-progress-wrap');
    const progBar  = document.getElementById('ui-lang-progress-bar');
    const progText = document.getElementById('ui-lang-progress-text');
    const progEta  = document.getElementById('ui-lang-progress-eta');
    const progCur  = document.getElementById('ui-lang-progress-current');
    if (!sel || !status) return;

    /* Re-sync dropdown with persisted choice (defends against the
       browser autorestoring an empty <select> on bfcache). */
    const saved = window.SLI18n.getLang();
    if (saved && [...sel.options].some(o => o.value === saved)) {
      sel.value = saved;
    }

    function fmtSecs(s) {
      if (!Number.isFinite(s) || s < 0) return '';
      if (s < 60) return Math.round(s) + 's';
      const m = Math.floor(s / 60);
      const rs = Math.round(s - m * 60);
      return m + 'm ' + (rs < 10 ? '0' : '') + rs + 's';
    }

    function showProgress(stats, currentText) {
      if (!progWrap || !stats) return;
      const total = Math.max(stats.toFetch || 0, 0);
      const done  = (stats.ok || 0) + (stats.failed || 0);
      const pct   = total > 0 ? (done / total * 100) : 0;
      progWrap.style.display = 'block';
      if (progBar) {
        progBar.max   = total > 0 ? total : 1;
        progBar.value = done;
      }
      if (progText) {
        const failTail = (stats.failed > 0)
          ? ' · ' + stats.failed + ' failed' : '';
        progText.textContent = done + ' / ' + total + ' strings (' +
          pct.toFixed(0) + '%)' + failTail;
      }
      if (progEta && stats.startedAt && done > 0 && done < total) {
        const elapsed = (Date.now() - stats.startedAt) / 1000;
        const rate = done / elapsed;
        const eta = (total - done) / rate;
        progEta.textContent = '~' + fmtSecs(eta) + ' left · ' +
                              rate.toFixed(1) + '/s';
      } else if (progEta) {
        progEta.textContent = '';
      }
      if (progCur) {
        progCur.textContent = currentText
          ? '› ' + (currentText.length > 60
                    ? currentText.slice(0, 60) + '…'
                    : currentText)
          : '';
      }
    }

    function hideProgressLater() {
      if (!progWrap) return;
      setTimeout(() => { progWrap.style.display = 'none'; }, 6000);
    }

    function reportStats(stats, lang, prefix) {
      if (!stats) { status.textContent = '✓ done'; hideProgressLater(); return; }
      const fetched = (stats.ok || 0) + (stats.failed || 0);
      const wallSecs = (Date.now() - (stats.startedAt || Date.now())) / 1000;
      if (stats.failed === 0 && fetched > 0) {
        status.textContent = `✓ ${prefix} — ${stats.ok} new strings translated in ${fmtSecs(wallSecs)} (${stats.cacheHits} from cache)`;
      } else if (stats.failed === 0 && fetched === 0) {
        status.textContent = `✓ ${prefix} from cache (${stats.cacheHits} strings, 0 network calls)`;
      } else {
        const why = stats.firstErr ? ` — ${stats.firstErr}` : '';
        status.textContent =
          `⚠ ${stats.ok}/${fetched} translated, ${stats.failed} failed${why}`;
      }
      hideProgressLater();
    }

    sel.addEventListener('change', async () => {
      status.textContent = '⏳ translating UI…';
      const lang = sel.value;
      try {
        if (lang === 'en') {
          await window.SLI18n.setLang('en');
          status.textContent = '✓ reverted to English';
          if (progWrap) progWrap.style.display = 'none';
        } else {
          const stats = await window.SLI18n.setLang(lang, showProgress);
          reportStats(stats, lang, 'applied');
        }
      } catch (e) {
        status.textContent = '✗ ' + (e.message || 'translate failed');
      }
    });

    if (clear) {
      clear.addEventListener('click', async () => {
        const lang = sel.value;
        if (lang === 'en') {
          status.textContent = 'pick a non-English language first';
          return;
        }
        status.textContent = '⏳ clearing cache + re-translating…';
        try {
          await window.SLI18n.clearCache(lang);
          const stats = await window.SLI18n.setLang(lang, showProgress);
          reportStats(stats, lang, 're-translated');
        } catch (e) {
          status.textContent = '✗ ' + (e.message || 'failed');
        }
      });
    }
  }

  /* Boot — apply saved language as soon as DOM is parsed. */
  function boot() {
    wireLangPicker();

    const lang = window.SLI18n.getLang();
    if (lang && lang !== 'en') {
      applyLang(lang, document.body).catch((e) =>
        console.warn('[i18n] initial translation failed', e));
    }

    /* MutationObserver — translate dynamically-inserted nodes
       (settings sub-sections, modals, picker rows). Coalesced so the
       observer doesn't fire applyLang once per added node — instead
       we batch all added subtrees for COALESCE_MS, then do ONE pass
       over the body so per-string translations get reused. */
    const COALESCE_MS = 300;
    let pending = new Set();
    let coalesceTimer = null;
    let applyInFlight = false;

    async function drain() {
      coalesceTimer = null;
      if (applyInFlight) {
        coalesceTimer = setTimeout(drain, COALESCE_MS);
        return;
      }
      pending.clear();
      const lang = window.SLI18n.getLang();
      if (!lang || lang === 'en') return;
      applyInFlight = true;
      try {
        await applyLang(lang, document.body);
      } catch (e) {
        console.warn('[i18n] subtree translate failed', e);
      } finally {
        applyInFlight = false;
      }
    }

    g_observer = new MutationObserver((muts) => {
      for (const m of muts) {
        for (const node of m.addedNodes) {
          if (node.nodeType === 1) pending.add(node);
        }
      }
      if (!coalesceTimer) coalesceTimer = setTimeout(drain, COALESCE_MS);
    });
    g_observer.observe(document.body, { childList: true, subtree: true });

    /* Override the public translateSubtree to use the same coalescing
       pipeline (instead of firing immediately and racing with picker). */
    const _prevTranslateSubtree = window.SLI18n.translateSubtree;
    window.SLI18n.translateSubtree = function (root) {
      if (root && root.nodeType === 1) pending.add(root);
      if (!coalesceTimer) coalesceTimer = setTimeout(drain, COALESCE_MS);
    };
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot);
  } else {
    boot();
  }
  console.log('[i18n] module ready, SLI18n =', window.SLI18n);

  } catch (err) {
    console.error('[i18n] module load failed — picker will not work:', err);
  }
})();
