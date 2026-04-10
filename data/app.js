/* ═══════════════════════════════════════════════════════════
   app.js  —  shared utilities for all attendance pages
   Every page includes: <script src="/app.js"></script>
   ═══════════════════════════════════════════════════════════ */

// ── Theme ──────────────────────────────────────────────────────────
(function () {
  var KEY = 'att_theme';
  function applyTheme(th) {
    document.body.classList.toggle('light', th === 'light');
    var btn = document.getElementById('theme-btn');
    if (btn) btn.textContent = th === 'light' ? '☽ Dark' : '☀ Light';
    // Sync toggle switch
    var track = document.getElementById('ts-track');
    var icon  = document.getElementById('ts-icon');
    if (track) track.classList.toggle('on', th === 'light');
    if (icon)  icon.textContent = th === 'light' ? '☽' : '☀';
  }
  applyTheme(localStorage.getItem(KEY) || 'dark');
  window.toggleTheme = function () {
    var cur = document.body.classList.contains('light') ? 'light' : 'dark';
    var nxt = cur === 'light' ? 'dark' : 'light';
    localStorage.setItem(KEY, nxt);
    applyTheme(nxt);
  };
})();

// ── Font / Color Customization ─────────────────────────────────────
(function () {
  // System-only font stacks — no external network requests (safe in AP mode)
  // FIX-02: Each entry now has a genuinely distinct font stack instead of
  // all resolving to the same Courier New fallback.
  const FONTS = {
    'default':    { name: 'Default Mono',   stack: "'Courier New', Courier, monospace" },
    'consolas':   { name: 'Consolas',       stack: "Consolas, 'Courier New', monospace" },
    'menlo':      { name: 'Menlo',          stack: "Menlo, Monaco, 'Courier New', monospace" },
    'monaco':     { name: 'Monaco',         stack: "Monaco, 'Courier New', monospace" },
    'sfmono':     { name: 'SF Mono',        stack: "'SF Mono', Menlo, monospace" },
    'lucida':     { name: 'Lucida Console', stack: "'Lucida Console', 'Courier New', monospace" },
  };
  window.FONTS = FONTS;

  function applyAppearance(cfg) {
    var root = document.documentElement;
    var font = FONTS[cfg.font] || FONTS['default'];
    root.style.setProperty('--font', font.stack);
    if (cfg.fontSize) root.style.setProperty('--font-size-base', cfg.fontSize + 'px');
  }

  function loadAppearance() {
    try {
      var cfg = JSON.parse(localStorage.getItem('att_appearance') || '{}');
      if (cfg.font || cfg.fontSize) applyAppearance(cfg);
    } catch (e) {}
  }

  window.saveAppearance = function (cfg) {
    localStorage.setItem('att_appearance', JSON.stringify(cfg));
    applyAppearance(cfg);
  };
  window.getAppearance = function () {
    try { return JSON.parse(localStorage.getItem('att_appearance') || '{}'); } catch(e) { return {}; }
  };

  loadAppearance();
  var _origToggle = window.toggleTheme;
  window.toggleTheme = function () { _origToggle(); loadAppearance(); };
})();

// ── Debounce utility ────────────────────────────────────────────────
window.debounce = function(fn, delay) {
  var timer;
  return function() {
    clearTimeout(timer);
    timer = setTimeout(fn, delay);
  };
};

// ── Session cache — sessionStorage-backed, one network hit per browser session ──
(function () {
  var _promise = null;
  var SESS_KEY = 'att_sess_v1';

  window.getSession = function (force) {
    if (!force) {
      try {
        var c = sessionStorage.getItem(SESS_KEY);
        if (c) { if (!_promise) _promise = Promise.resolve(JSON.parse(c)); return _promise; }
      } catch (e) {}
    }
    _promise = null;
    _promise = api('/api/session').then(function (s) {
      try { sessionStorage.setItem(SESS_KEY, JSON.stringify(s)); } catch (e) {}
      return s;
    });
    return _promise;
  };

  window.invalidateSession = function () {
    _promise = null;
    try { sessionStorage.removeItem(SESS_KEY); } catch (e) {}
  };
})();

// ── Classes cache — sessionStorage-backed ─────────────────────────
(function () {
  var _promise = null;
  var CLS_KEY = 'att_classes_v1';

  window.getClasses = function (force) {
    if (!force) {
      try {
        var c = sessionStorage.getItem(CLS_KEY);
        // BUG-A FIX: Only serve the cache if it is non-empty.
        // When the SD card crashed, /api/classes returned [] and that empty
        // array was cached here. Every page load then read the stale [] and
        // never contacted the server again -- even after the ESP32 was fixed
        // or a backup was restored. An empty list is a transient failure state,
        // not a valid steady state, so treat it as a cache miss and re-fetch.
        if (c) {
          var parsed = JSON.parse(c);
          if (parsed && parsed.length > 0) {
            if (!_promise) _promise = Promise.resolve(parsed);
            return _promise;
          }
          // Stale empty cache -- remove it and fall through to re-fetch
          sessionStorage.removeItem(CLS_KEY);
        }
      } catch (e) {}
    }
    _promise = null;
    _promise = api('/api/classes').then(function (classes) {
      // Only persist a non-empty result -- an empty array means the SD card
      // is not ready yet; we never want to lock the browser into showing nothing.
      if (classes && classes.length > 0) {
        try { sessionStorage.setItem(CLS_KEY, JSON.stringify(classes)); } catch (e) {}
      }
      return classes;
    });
    return _promise;
  };

  window.invalidateClasses = function () {
    _promise = null;
    try { sessionStorage.removeItem(CLS_KEY); } catch (e) {}
  };
})();

// ── Navbar / drawer ────────────────────────────────────────────────
(function () {
  document.addEventListener('DOMContentLoaded', function () {
    var btn     = document.getElementById('mb');
    var drawer  = document.getElementById('drawer');
    var overlay = document.getElementById('overlay');
    if (!btn || !drawer || !overlay) return;

    function toggle(open) {
      btn.setAttribute('aria-expanded', open ? 'true' : 'false');
      btn.classList.toggle('open', open);
      drawer.classList.toggle('open', open);
      overlay.classList.toggle('open', open);
      document.body.style.overflow = open ? 'hidden' : '';
    }
    btn.addEventListener('click', function () { toggle(!drawer.classList.contains('open')); });
    overlay.addEventListener('click', function () { toggle(false); });
    document.addEventListener('keydown', function (e) { if (e.key === 'Escape') toggle(false); });

    // Close drawer on any nav link click (for SPA-like feel)
    drawer.addEventListener('click', function (e) {
      if (e.target.closest('a')) toggle(false);
    });
  });
})();

// ── Logout beacon on tab close ─────────────────────────────────────
// NOTE: Auto-logout on visibilitychange was removed because the event
// fires on every page navigation, which destroyed the session whenever
// the user moved between pages (login → select-class → attendance).
// Session expiry is handled server-side with a 30-minute timeout.

// ── Toast ──────────────────────────────────────────────────────────
window.showToast = function (msg, isError) {
  let el = document.getElementById('toast');
  if (!el) {
    el = document.createElement('div');
    el.id = 'toast';
    el.className = 'toast';
    document.body.appendChild(el);
  }
  el.textContent = msg;
  el.classList.toggle('error', !!isError);
  el.classList.add('show');
  clearTimeout(el._timer);
  el._timer = setTimeout(function () { el.classList.remove('show'); }, 3200);
};

// ── Confirm-delete modal ───────────────────────────────────────────
// FIX-07: Use a fully dynamic text element so the prefix is not hardcoded.
window.confirmAction = function (label, onConfirm) {
  const bg      = document.getElementById('confirm-modal');
  const textEl  = document.getElementById('confirm-text');
  const yesBtn  = document.getElementById('confirm-yes-btn');
  if (!bg || !textEl || !yesBtn) return;
  textEl.textContent = label;
  yesBtn.onclick = function () {
    bg.classList.remove('show');
    onConfirm();
  };
  bg.classList.add('show');
};

// FIX-03: Use api(url, 'DELETE') instead of window.location.href (which
// issued a GET against a destructive endpoint and had no error handling).
window.confirmDel = function (url, label, onDone) {
  window.confirmAction(label, function () {
    api(url, 'DELETE')
      .then(function () { if (onDone) onDone(); })
      .catch(function (e) { showToast(e.message, true); });
  });
};
window.confirmFetch = function (url, label, method, body, onDone) {
  window.confirmAction(label, function () {
    api(url, method || 'GET', body).then(function (res) { if (onDone) onDone(res); });
  });
};

// ── Modal helpers ──────────────────────────────────────────────────
window.openModal  = function (id) { const el = document.getElementById(id); if (el) el.classList.add('show');    };
window.closeModal = function (id) { const el = document.getElementById(id); if (el) el.classList.remove('show'); };

document.addEventListener('DOMContentLoaded', function () {
  document.querySelectorAll('.modal-bg, .confirm-bg').forEach(function (el) {
    el.addEventListener('click', function (e) {
      if (e.target === el) el.classList.remove('show');
    });
  });
});

// ── Tab bar (FIXED: scoped to nearest .tab-bar ancestor) ───────────
window.initTabs = function (tabIds, defaultTab, containerEl) {
  // containerEl: optional DOM element to scope button search to
  const scope = containerEl || document;
  // FIX-04: Removed dead `bar` variable (was assigned but never used).

  // Find the tab-bar that owns these tabs' buttons
  // Get all buttons that have data-tab matching one of our tabIds
  const validSet = {};
  tabIds.forEach(function (t) { validSet[t] = true; });

  const allBtns = (scope && scope !== document)
    ? scope.querySelectorAll('.tab-bar button[data-tab]')
    : document.querySelectorAll('.tab-bar button[data-tab]');

  const btns = Array.from(allBtns).filter(function (b) { return validSet[b.dataset.tab]; });
  // Find pill from closest tab-bar of first matching button
  const pill = btns.length ? btns[0].closest('.tab-bar').querySelector('.tab-pill') : null;

  function showTab(id) {
    tabIds.forEach(function (tid) {
      const el = document.getElementById(tid);
      if (el) el.style.display = tid === id ? 'block' : 'none';
    });
    btns.forEach(function (btn) {
      btn.classList.toggle('active', btn.dataset.tab === id);
    });
    if (pill) {
      const active = btns.find(function (b) { return b.dataset.tab === id; });
      if (active) {
        pill.style.left  = active.offsetLeft + 'px';
        pill.style.width = active.offsetWidth + 'px';
      }
    }
  }

  btns.forEach(function (btn) {
    btn.addEventListener('click', function () { showTab(btn.dataset.tab); });
  });
  setTimeout(function () { showTab(defaultTab); }, 0);
};

// ── Date groups (collapsible log entries) ──────────────────────────
window.initGroups = function () {
  document.querySelectorAll('.date-group').forEach(function (grp) {
    if (grp.dataset.gi) return;
    grp.dataset.gi = '1';
    grp.classList.add('collapsed');
    const hdr = grp.querySelector('.date-group-hdr');
    if (hdr) {
      hdr.addEventListener('click', function (e) {
        e.preventDefault();
        e.stopPropagation();
        grp.classList.toggle('collapsed');
      });
    }
  });
};

// ── SSE Helper ─────────────────────────────────────────────────────
window.createSSE = function (url, onMessage, onError) {
  let es = null;
  let retryTimer = null;
  let stopped = false;

  function connect() {
    if (stopped) return;
    try {
      es = new EventSource(url);
      es.onmessage = function (e) {
        try { onMessage(JSON.parse(e.data)); } catch (x) { onMessage(e.data); }
      };
      es.onerror = function () {
        es.close();
        if (onError) onError();
        if (!stopped) retryTimer = setTimeout(connect, 5000);
      };
    } catch (e) {
      if (!stopped) retryTimer = setTimeout(connect, 5000);
    }
  }

  connect();

  return {
    stop: function () {
      stopped = true;
      clearTimeout(retryTimer);
      if (es) es.close();
    }
  };
};

/* ── apiStreamAttendance — FIXED & ROBUST nested attendance streaming ─── */
window.apiStreamAttendance = function (url, onHeader, onTodayItem, onLogGroup, onDone, onError) {
  fetch(url, { headers: { 'Content-Type': 'application/json' } })
    .then(function (r) {
      if (r.status === 401) { window.location.href = '/login.html'; return; }
      if (r.status === 403) { window.location.href = '/select-class.html'; return; }
      if (!r.ok) {
        r.json().catch(() => {}).then(j => onError(new Error(j.error || 'HTTP ' + r.status)));
        return;
      }

      const reader = r.body.getReader();
      const dec = new TextDecoder();
      let buf = '';
      let phase = 'header';   // header → today → between → log

      let pos = 0, depth = 0, objStart = -1, inStr = false, esc = false;

      function resetExtractor() {
        pos = 0; depth = 0; objStart = -1; inStr = false; esc = false;
      }

      function processBuffer() {

        /* Phase: header */
        if (phase === 'header') {
          const mark = '"today":[';
          const idx = buf.indexOf(mark);
          if (idx < 0) return;

          const head = buf.slice(0, idx);
          const dateM = head.match(/"today_date"\s*:\s*"([^"]+)"/);
          const statsM = head.match(/"stats"\s*:\s*(\{[^}]+\})/);

          try {
            onHeader({
              today_date: dateM ? dateM[1] : '',
              stats: statsM ? JSON.parse(statsM[1]) : {}
            });
          } catch (e) {
            onHeader({ today_date: '', stats: {} });
          }

          buf = buf.slice(idx + mark.length);
          phase = 'today';
          resetExtractor();
          processBuffer();
          return;
        }

        /* Phase: today array */
        if (phase === 'today') {
          while (pos < buf.length) {
            const c = buf[pos];
            if (esc) { esc = false; pos++; continue; }
            if (c === '\\' && inStr) { esc = true; pos++; continue; }
            if (c === '"') { inStr = !inStr; pos++; continue; }
            if (inStr) { pos++; continue; }

            if (c === '{') {
              if (depth === 0) objStart = pos;
              depth++;
            } else if (c === '}') {
              depth--;
              if (depth === 0 && objStart >= 0) {
                try { onTodayItem(JSON.parse(buf.slice(objStart, pos + 1))); } catch (e) {}
                buf = buf.slice(pos + 1);
                pos = -1;
                objStart = -1;
              }
            } else if (c === ']' && depth === 0) {
              buf = buf.slice(pos + 1);
              phase = 'between';
              resetExtractor();
              processBuffer();
              return;
            }
            pos++;
          }
          if (objStart >= 0) { buf = buf.slice(objStart); pos = buf.length; objStart = 0; }
          else { buf = ''; pos = 0; }
          return;
        }

        /* Phase: between → look for "log" (very tolerant) */
        if (phase === 'between') {
          // Look for "log" anywhere (handles , "log" or ,"log" or whitespace)
          const logIdx = buf.indexOf('"log"');
          if (logIdx < 0) return;

          // Skip past the "log" key and the colon + opening bracket
          const afterLog = buf.slice(logIdx + 6); // skip "log"
          const openBracket = afterLog.indexOf('[');
          if (openBracket < 0) return;

          buf = afterLog.slice(openBracket + 1);
          phase = 'log';
          resetExtractor();
          processBuffer();
          return;
        }

        /* Phase: log array */
        if (phase === 'log') {
          while (pos < buf.length) {
            const c = buf[pos];
            if (esc) { esc = false; pos++; continue; }
            if (c === '\\' && inStr) { esc = true; pos++; continue; }
            if (c === '"') { inStr = !inStr; pos++; continue; }
            if (inStr) { pos++; continue; }

            if (c === '{') {
              if (depth === 0) objStart = pos;
              depth++;
            } else if (c === '}') {
              depth--;
              if (depth === 0 && objStart >= 0) {
                try {
                  const group = JSON.parse(buf.slice(objStart, pos + 1));
                  onLogGroup(group);
                } catch (e) {}
                buf = buf.slice(pos + 1);
                pos = -1;
                objStart = -1;
              }
            } else if (c === ']' && depth === 0) {
              /* End of log array (handles empty [] and normal close) */
              buf = buf.slice(pos + 1);
              phase = 'done';
              break;
            }
            pos++;
          }
          if (objStart >= 0) { buf = buf.slice(objStart); pos = buf.length; objStart = 0; }
          else { buf = ''; pos = 0; }
        }
      }

      function pump() {
        reader.read().then(res => {
          if (res.done) { onDone(); return; }
          buf += dec.decode(res.value, { stream: true });
          processBuffer();
          pump();
        }).catch(onError);
      }

      pump();
    })
    .catch(onError);
};

/* ── apiStreamReport — streams { class_days, min_pct, rows:[{...},...] } ── */
window.apiStreamReport = function (url, onHeader, onRow, onDone, onError) {
  fetch(url, { headers: { 'Content-Type': 'application/json' } })
    .then(function (r) {
      if (r.status === 401) { window.location.href = '/login.html'; return; }
      if (r.status === 403) { window.location.href = '/select-class.html'; return; }
      if (!r.ok) {
        r.json().catch(function () { return {}; }).then(function (j) {
          onError(new Error(j.error || 'HTTP ' + r.status));
        });
        return;
      }

      var reader = r.body.getReader();
      var dec    = new TextDecoder();
      var buf    = '';
      var phase  = 'header'; /* 'header' → 'rows' */
      var pos = 0, depth = 0, objStart = -1, inStr = false, esc = false;

      function resetExtractor() {
        pos = 0; depth = 0; objStart = -1; inStr = false; esc = false;
      }

      function processBuffer() {
        /* ── Phase: header — wait for "rows":[ marker ── */
        if (phase === 'header') {
          var mark = '"rows":[';
          var idx  = buf.indexOf(mark);
          if (idx < 0) return;
          var head  = buf.slice(0, idx);
          var daysM = head.match(/"class_days"\s*:\s*(\d+)/);
          var pctM  = head.match(/"min_pct"\s*:\s*(\d+)/);
          onHeader({
            class_days: daysM ? parseInt(daysM[1]) : 0,
            min_pct:    pctM  ? parseInt(pctM[1])  : 75
          });
          buf = buf.slice(idx + mark.length);
          phase = 'rows';
          resetExtractor();
          processBuffer();
          return;
        }

        /* ── Phase: rows — extract each {…} row object ── */
        while (pos < buf.length) {
          var c = buf[pos];
          if (esc)                  { esc = false;    pos++; continue; }
          if (c === '\\' && inStr)  { esc = true;     pos++; continue; }
          if (c === '"')            { inStr = !inStr; pos++; continue; }
          if (inStr)                {                  pos++; continue; }
          if (c === '{') {
            if (depth === 0) objStart = pos;
            depth++;
          } else if (c === '}') {
            depth--;
            if (depth === 0 && objStart >= 0) {
              try { onRow(JSON.parse(buf.slice(objStart, pos + 1))); } catch (e) {}
              buf = buf.slice(pos + 1); pos = -1; objStart = -1;
            }
          }
          pos++;
        }
        if (objStart >= 0) { buf = buf.slice(objStart); pos = buf.length; objStart = 0; }
        else               { buf = ''; pos = 0; }
      }

      function pump() {
        reader.read().then(function (res) {
          if (res.done) { onDone(); return; }
          buf += dec.decode(res.value, { stream: true });
          processBuffer();
          pump();
        }).catch(onError);
      }
      pump();
    }).catch(onError);
};

// ── Core API helper ────────────────────────────────────────────────
window.api = function (url, method, body) {
  method = method || 'GET';
  const opts = {
    method: method,
    headers: { 'Content-Type': 'application/json' },
  };
  if (body && method !== 'GET') opts.body = JSON.stringify(body);
  return fetch(url, opts).then(function (r) {
    if (r.status === 401) { window.location.href = '/login.html'; throw new Error('unauthorized'); }
    if (r.status === 403) { window.location.href = '/select-class.html'; throw new Error('no_class'); }
    if (!r.ok) {
      return r.json().catch(function () { return {}; }).then(function (j) {
        throw new Error(j.error || ('HTTP ' + r.status));
      });
    }
    return r.json();
  });
};

window.apiUpload = function (url, formData) {
  return fetch(url, { method: 'POST', body: formData }).then(function (r) {
    if (r.status === 401) { window.location.href = '/login.html'; throw new Error('unauthorized'); }
    if (!r.ok) return r.json().then(function (j) { throw new Error(j.error || 'Upload failed'); });
    return r.json();
  });
};

// ── Status cache (5-minute TTL for NTP check) ──────────────────────
var _statusCache = null, _statusTs = 0;
function getStatus() {
  var now = Date.now();
  if (_statusCache && now - _statusTs < 300000) return Promise.resolve(_statusCache);
  return api('/api/status').then(function (s) { _statusCache = s; _statusTs = Date.now(); return s; });
}

// ── Navbar builder ─────────────────────────────────────────────────
window.buildNavbar = function (activeId) {
  // Preload in parallel — results will be cached by the time the callbacks run
  getSession();
  getClasses();

  var links = [
    { id: 'classes',    href: '/select-class.html',  ic: '⊟',  label: 'Classes'      },
    { id: 'admin',      href: '/admin.html',          ic: '⚙',  label: 'Admin Panel'  },
    { id: 'register',   href: '/register.html',       ic: '+',  label: 'Registration' },
    { id: 'students',   href: '/students.html',       ic: '≡',  label: 'Students'     },
    { id: 'attendance', href: '/attendance.html',     ic: '✓',  label: 'Attendance'   },
    { id: 'report',     href: '/report.html',         ic: '⌖',  label: 'Report'       },
    { id: 'wifi',       href: '/wifi-config.html',    ic: '◈',  label: 'WiFi Config'  },
  ];

  // Build institution brand in navbar
  var brand = document.querySelector('.nb-brand');
  if (brand) {
    brand.innerHTML = '<div class="nb-logo">■</div>' +
      '<span class="nb-title">R<span class="nb-at">@</span>sel\'s math home</span>';
    brand.href = '/select-class.html';
  }

  // Fetch session — determine if class is selected
  getSession().then(function (sess) {
    var hasClass = !!(sess && sess.class);

    // FIX-36: Check NTP sync status and show a banner if the clock is unsynced.
    // Attendance timestamps will be wrong (year 2000) without a successful sync.
    getStatus().then(function (s) {
      if (s && s.ntp_ok === false) {
        var existing = document.getElementById('ntp-warning-banner');
        if (!existing) {
          var banner = document.createElement('div');
          banner.id = 'ntp-warning-banner';
          banner.style.cssText =
            'position:fixed;top:0;left:0;right:0;z-index:9999;' +
            'background:#b45309;color:#fff;font-size:.72rem;' +
            'letter-spacing:.06em;text-align:center;padding:.45rem .75rem;';
          banner.textContent =
            '\u26a0 Clock not synced \u2014 attendance timestamps may be incorrect. ' +
            'Connect to WiFi to enable NTP sync.';
          document.body.insertBefore(banner, document.body.firstChild);
        }
      }
    }).catch(function () {});

    // Class selector dropdown in navbar
    var clsSel = document.querySelector('.nb-cls-sel');
    var themeBtn = document.getElementById('theme-btn');
    var themeSwitch = document.querySelector('.theme-switch');

    if (hasClass) {
      // Show appearance and class controls
      if (themeBtn)    themeBtn.classList.add('visible');
      if (themeSwitch) themeSwitch.classList.add('visible');
      if (clsSel)      clsSel.classList.add('visible');
    }

    // Populate class selector
    getClasses().then(function (classes) {
      if (!clsSel) return;
      var sel = clsSel.querySelector('select');
      if (!sel) return;
      sel.innerHTML = classes.map(function (c) {
        var cn = typeof c === 'object' ? c.class_num : c;
        return '<option value="' + cn + '"' + (sess.class == cn ? ' selected' : '') + '>Class ' + cn + '</option>';
      }).join('');

      // Change class on select
      sel.addEventListener('change', function () {
        var cn = sel.value;
        // Optimistic: store locally so attendance.html can read immediately
        try { sessionStorage.setItem('att_active_class', String(cn)); } catch (e) {}
        invalidateSession(); // force re-fetch so g_class is fresh
        api('/api/set-class?c=' + cn)
          .catch(function (e) { showToast(e.message, true); });
        location.href = '/attendance.html'; // navigate without waiting
      });
    }).catch(function () {});

  }).catch(function () { location.href = '/login.html'; });

  // Build drawer
  var drawer = document.getElementById('drawer');
  if (!drawer) return;

  drawer.innerHTML =
    '<div class="drawer-header">' +
      '<span class="drawer-brand">R@sel\'s math home</span>' +
      '<button class="drawer-close" onclick="(function(){document.getElementById(\'drawer\').classList.remove(\'open\');document.getElementById(\'mb\').classList.remove(\'open\');document.getElementById(\'overlay\').classList.remove(\'open\');document.body.style.overflow=\'\'})()">✕ Close</button>' +
    '</div>' +
    '<div class="drawer-nav">' +
    links.map(function (l) {
      return '<a href="' + l.href + '"' + (l.id === activeId ? ' class="active"' : '') + '>' +
        '<span class="ic">' + l.ic + '</span>' +
        '<span class="link-label">' + l.label + '</span></a>';
    }).join('') +
    '</div>' +
    '<div class="drawer-footer">' +
      '<a href="#" onclick="api(\'/api/logout\',\'POST\').then(function(){location.href=\'/login.html\'}).catch(function(){location.href=\'/login.html\'})">' +
        '<span class="ic">✕</span><span class="link-label">Logout</span>' +
      '</a>' +
    '</div>';
};
