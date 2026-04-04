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
  const FONTS = {
    'jetbrains':   { name: 'JetBrains Mono',    stack: "'Courier New', Courier, monospace" },
    'firacode':    { name: 'Fira Code',          stack: "'Courier New', Courier, monospace" },
    'ibmplex':     { name: 'IBM Plex Mono',      stack: "'Courier New', Courier, monospace" },
    'spacemono':   { name: 'Space Mono',         stack: "'Courier New', Courier, monospace" },
    'robotomono':  { name: 'Roboto Mono',        stack: "'Courier New', Courier, monospace" },
    'sourcecode':  { name: 'Source Code Pro',    stack: "'Courier New', Courier, monospace" },
    'dmmono':      { name: 'DM Mono',            stack: "'Courier New', Courier, monospace" },
    'inconsolata': { name: 'Inconsolata',        stack: "'Courier New', Courier, monospace" },
    'courierprime':{ name: 'Courier Prime',      stack: "'Courier New', Courier, monospace" },
    'azeret':      { name: 'Azeret Mono',        stack: "'Courier New', Courier, monospace" },
  };
  window.FONTS = FONTS;

  function applyAppearance(cfg) {
    var root = document.documentElement;
    var font = FONTS[cfg.font] || FONTS['jetbrains'];
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

// ── Session cache — one request per page, shared between buildNavbar and page code ──
(function () {
  var _promise = null;
  window.getSession = function () {
    if (!_promise) _promise = api('/api/session');
    return _promise;
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
window.addEventListener('pagehide', function (e) {
  if (e.persisted) return;
  if (sessionStorage.getItem('reloading') === '1') {
    sessionStorage.removeItem('reloading');
    return;
  }
  navigator.sendBeacon('/api/logout');
});
window.addEventListener('beforeunload', function () {
  sessionStorage.setItem('reloading', '1');
});

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
window.confirmAction = function (label, onConfirm) {
  const bg      = document.getElementById('confirm-modal');
  const labelEl = document.getElementById('confirm-label');
  const yesBtn  = document.getElementById('confirm-yes-btn');
  if (!bg || !labelEl || !yesBtn) return;
  labelEl.textContent = label;
  yesBtn.onclick = function () {
    bg.classList.remove('show');
    onConfirm();
  };
  bg.classList.add('show');
};

window.confirmDel   = function (url, label) {
  window.confirmAction(label, function () { window.location.href = url; });
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
  const bar   = typeof containerEl === 'string'
    ? document.getElementById(containerEl)
    : containerEl;

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

// ── Streaming helpers ───────────────────────────────────────────────
/* ── apiStream — flat JSON array streaming ─── */
window.apiStream = function (url, onItem, onDone, onError) {
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

      var reader   = r.body.getReader();
      var dec      = new TextDecoder();
      var buf      = '';
      var pos      = 0;
      var depth    = 0;
      var objStart = -1;
      var inStr    = false;
      var esc      = false;

      function pump() {
        reader.read().then(function (res) {
          if (res.done) { onDone(); return; }

          buf += dec.decode(res.value, { stream: true });

          /* Extract all complete {…} objects from buf */
          while (pos < buf.length) {
            var c = buf[pos];
            if (esc)               { esc = false;     pos++; continue; }
            if (c === '\\' && inStr) { esc = true;    pos++; continue; }
            if (c === '"')         { inStr = !inStr;  pos++; continue; }
            if (inStr)             {                   pos++; continue; }

            if (c === '{') {
              if (depth === 0) objStart = pos;
              depth++;
            } else if (c === '}') {
              depth--;
              if (depth === 0 && objStart >= 0) {
                try { onItem(JSON.parse(buf.slice(objStart, pos + 1))); } catch (e) {}
                buf      = buf.slice(pos + 1);
                pos      = -1;
                objStart = -1;
              }
            }
            pos++;
          }

          /* Keep only the in-progress object; discard already-processed bytes */
          if (objStart >= 0) { buf = buf.slice(objStart); pos = buf.length; objStart = 0; }
          else               { buf = ''; pos = 0; }

          pump();
        }).catch(onError);
      }
      pump();
    }).catch(onError);
};

/* ── apiStreamAttendance — nested attendance response streaming ─── */
window.apiStreamAttendance = function (url, onHeader, onTodayItem, onLogItem, onDone, onError) {
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
      /* phase: 'header' → 'today' → 'between' → 'log' */
      var phase  = 'header';

      /* Object extractor state — reset at each phase transition */
      var pos = 0, depth = 0, objStart = -1, inStr = false, esc = false;

      function resetExtractor() {
        pos = 0; depth = 0; objStart = -1; inStr = false; esc = false;
      }

      function processBuffer() {

        /* ── Phase: header ───────────────────────────────────────── */
        if (phase === 'header') {
          var mark = '"today":[';
          var idx  = buf.indexOf(mark);
          if (idx < 0) return; /* not enough data yet */

          var head   = buf.slice(0, idx);
          var dateM  = head.match(/"today_date"\s*:\s*"([^"]+)"/);
          var statsM = head.match(/"stats"\s*:\s*(\{[^}]+\})/);
          try {
            onHeader({
              today_date: dateM  ? dateM[1]             : '',
              stats:      statsM ? JSON.parse(statsM[1]) : {}
            });
          } catch (e) {
            onHeader({ today_date: '', stats: {} });
          }

          buf = buf.slice(idx + mark.length);
          phase = 'today';
          resetExtractor();
          processBuffer(); /* re-enter with remaining buffer */
          return;
        }

        /* ── Phase: today ────────────────────────────────────────── */
        if (phase === 'today') {
          while (pos < buf.length) {
            var c = buf[pos];
            if (esc)                 { esc = false;    pos++; continue; }
            if (c === '\\' && inStr) { esc = true;     pos++; continue; }
            if (c === '"')           { inStr = !inStr; pos++; continue; }
            if (inStr)               {                  pos++; continue; }

            if (c === '{') {
              if (depth === 0) objStart = pos;
              depth++;
            } else if (c === '}') {
              depth--;
              if (depth === 0 && objStart >= 0) {
                try { onTodayItem(JSON.parse(buf.slice(objStart, pos + 1))); } catch (e) {}
                buf = buf.slice(pos + 1); pos = -1; objStart = -1;
              }
            } else if (c === ']' && depth === 0) {
              /* Closing bracket of today array — switch phase */
              buf = buf.slice(pos + 1);
              phase = 'between';
              resetExtractor();
              processBuffer();
              return;
            }
            pos++;
          }
          if (objStart >= 0) { buf = buf.slice(objStart); pos = buf.length; objStart = 0; }
          else               { buf = ''; pos = 0; }
          return;
        }

        /* ── Phase: between (looking for "log":[) ────────────────── */
        if (phase === 'between') {
          var mark2 = '"log":[';
          var idx2  = buf.indexOf(mark2);
          if (idx2 < 0) return;
          buf = buf.slice(idx2 + mark2.length);
          phase = 'log';
          resetExtractor();
          processBuffer();
          return;
        }

        /* ── Phase: log ──────────────────────────────────────────── */
        if (phase === 'log') {
          while (pos < buf.length) {
            var c = buf[pos];
            if (esc)                 { esc = false;    pos++; continue; }
            if (c === '\\' && inStr) { esc = true;     pos++; continue; }
            if (c === '"')           { inStr = !inStr; pos++; continue; }
            if (inStr)               {                  pos++; continue; }

            if (c === '{') {
              if (depth === 0) objStart = pos;
              depth++;
            } else if (c === '}') {
              depth--;
              if (depth === 0 && objStart >= 0) {
                try { onLogItem(JSON.parse(buf.slice(objStart, pos + 1))); } catch (e) {}
                buf = buf.slice(pos + 1); pos = -1; objStart = -1;
              }
            }
            pos++;
          }
          if (objStart >= 0) { buf = buf.slice(objStart); pos = buf.length; objStart = 0; }
          else               { buf = ''; pos = 0; }
        }
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

// ── Navbar builder ─────────────────────────────────────────────────
window.buildNavbar = function (activeId) {
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
    api('/api/classes').then(function (classes) {
      if (!clsSel) return;
      var sel = clsSel.querySelector('select');
      if (!sel) return;
      sel.innerHTML = classes.map(function (c) {
        var cn = typeof c === 'object' ? c.class_num : c;
        return '<option value="' + cn + '"' + (sess.class == cn ? ' selected' : '') + '>Class ' + cn + '</option>';
      }).join('');

      // Change class on select
      sel.addEventListener('change', function () {
        api('/api/set-class?c=' + sel.value).then(function () {
          location.href = '/attendance.html';
        }).catch(function (e) { showToast(e.message, true); });
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
