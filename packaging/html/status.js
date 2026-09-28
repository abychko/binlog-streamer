(function () {
  'use strict';
  var $ = function (id) { return document.getElementById(id); };
  var pad = function (n) { return n < 10 ? '0' + n : '' + n; };

  // Thresholds are for colour only; the numbers are always shown.
  var LAG_WARN_S = 5, LAG_BAD_S = 60;
  var FILL_WARN = 80, FILL_BAD = 95;

  function time(unix) {
    if (unix == null) return '–';
    var d = new Date(unix * 1000);
    return d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' +
      pad(d.getDate()) + ' ' + pad(d.getHours()) + ':' + pad(d.getMinutes()) +
      ':' + pad(d.getSeconds());
  }
  function duration(s) {
    if (s == null) return '–';
    var d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600),
        m = Math.floor(s % 3600 / 60), sec = s % 60;
    return (d ? d + 'd ' : '') + pad(h) + ':' + pad(m) + ':' + pad(sec);
  }
  function ago(unix, now) {
    if (unix == null || now == null) return '';
    return duration(Math.max(0, now - unix));
  }
  function bytes(n) {
    if (n == null) return '–';
    var units = ['B', 'KiB', 'MiB', 'GiB', 'TiB'], i = 0, v = n;
    while (v >= 1024 && i < units.length - 1) { v /= 1024; i++; }
    return (i ? v.toFixed(i > 1 ? 2 : 1) : v) + ' ' + units[i];
  }
  function bytesLong(n) {
    return n == null ? '–' : bytes(n) + (n >= 1024 ?
      ' (' + n.toLocaleString() + ' B)' : '');
  }
  function percent(n, max) {
    return max ? 100 * n / max : null;
  }
  function fillClass(p) {
    return p == null ? '' : p >= FILL_BAD ? 'bad' : p >= FILL_WARN ? 'warn'
      : 'ok';
  }
  function lagClass(seconds, caughtUp) {
    if (caughtUp) return 'ok';
    if (seconds == null) return 'neutral';
    return seconds >= LAG_BAD_S ? 'bad' : seconds >= LAG_WARN_S ? 'warn'
      : 'ok';
  }
  function lagText(seconds, caughtUp) {
    if (caughtUp) return 'caught up';
    if (seconds == null) return 'unknown';
    return duration(seconds) + ' behind';
  }
  function position(file, pos) {
    return file == null ? '–' : file + ':' + pos;
  }
  // A server replica sends program_name "mysqld"; anything else (a relay,
  // mysqlbinlog) is named, so its version is not read as a server's.
  function replicaVersion(r) {
    if (!r.version) return '–';
    return r.program && r.program !== 'mysqld' ?
      r.program + ' ' + r.version : r.version;
  }
  function text(id, value) { $(id).textContent = value; }
  function html(id, value) { $(id).innerHTML = value; }
  function badge(cls, label) {
    return '<span class="badge ' + cls + '">' + esc(label) + '</span>';
  }
  function esc(s) {
    return String(s).replace(/[&<>"]/g, function (c) {
      return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c];
    });
  }
  function fill(id, used, max) {
    var p = percent(used, max), bar = $(id + '-bar');
    bar.className = 'bar ' + fillClass(p);
    bar.firstElementChild.style.width = (p == null ? 0 : Math.min(100, p)) +
      '%';
    text(id, p == null ? bytes(used) : p.toFixed(1) + '%');
    text(id + '-used', bytes(used) + ' used');
    text(id + '-max', max ? 'of ' + bytes(max) : 'no limit');
  }
  function stateClass(state) {
    return state === 'serving' ? 'ok' : state === 'starting' ? 'warn' : 'bad';
  }

  function renderSource(d) {
    var s = d.source, cls, label;
    if (s.connected) { cls = 'ok'; label = 'connected'; }
    else { cls = 'bad'; label = 'DISCONNECTED'; }
    html('t-src', badge(cls, label) + ' ' +
      (s.connected ? badge(lagClass(s.behind_seconds, s.caught_up),
        lagText(s.behind_seconds, s.caught_up))
        : (s.attempt ? '<span class="muted aside">attempt ' + s.attempt +
          '</span>' : '')));
    text('t-src-addr', s.address);
    html('src-conn', badge(cls, label) + (s.connected ? ' ' +
      (s.tls ? badge('ok', 'TLS') : badge('warn', 'plain')) +
      (s.compression && s.compression !== 'none' ?
        ' ' + badge('neutral', s.compression) : '') :
      (s.attempt ? ' <span class="muted">reconnecting, attempt ' + s.attempt +
        '</span>' : '')));
    text('src-addr', s.address);
    text('src-since', s.since == null ? '–' :
      time(s.since) + '  (' + ago(s.since, d.now) + ')');
    text('src-server', s.server_id == null ? '–' :
      'server_id ' + s.server_id + ', ' + (s.version || '?'));
    text('src-uuid', s.server_uuid || '–');
    text('src-pos', position(s.file, s.position));
    text('src-ts', time(s.timestamp));
    html('src-lag', badge(lagClass(s.behind_seconds, s.caught_up),
      lagText(s.behind_seconds, s.caught_up)) + (s.clock == null ? '' :
      ' <span class="muted">source clock ' + esc(time(s.clock)) + '</span>'));
  }

  function renderStorage(d) {
    var st = d.storage, m = d.memory;
    fill('t-disk', st.bytes, st.max_bytes);
    fill('t-mem', m.bytes, m.max_bytes);
    text('st-pos', position(st.file, st.position));
    text('st-lag', st.behind_bytes == null ? '–' :
      st.behind_bytes === 0 ? 'nothing' : bytesLong(st.behind_bytes));
    text('st-files', st.files);
    text('st-bytes', bytesLong(st.bytes) +
      (st.max_bytes ? ' of ' + bytes(st.max_bytes) : ''));
    text('mem-bytes', bytesLong(m.bytes) +
      (m.max_bytes ? ' of ' + bytes(m.max_bytes) : ''));
    text('mem-files', m.files);
  }

  function renderReplicas(d) {
    var reps = d.replicas, worst = null, streaming = 0, catching = 0;
    reps.forEach(function (r) {
      if (r.state === 'streaming') streaming++;
      else if (r.state === 'catching_up') catching++;
      if (r.behind_seconds != null && (worst == null ||
          r.behind_seconds > worst)) worst = r.behind_seconds;
    });
    text('t-rep', reps.length);
    text('t-rep-sub', 'of ' + d.max_connections);
    var parts = [];
    if (streaming) parts.push(streaming + ' streaming');
    if (catching) parts.push(catching + ' catching up');
    if (reps.length - streaming - catching)
      parts.push((reps.length - streaming - catching) + ' connected');
    html('t-rep-lag', reps.length === 0 ? 'none connected' :
      esc(parts.join(', ')) + (!worst ? '' :
      ' · worst lag ' + badge(lagClass(worst, false), duration(worst))));
    text('rep-count', reps.length + ' / ' + d.max_connections);

    var rows = $('rep-rows');
    rows.innerHTML = '';
    if (!reps.length) {
      rows.innerHTML = '<tr><td colspan="8" class="muted empty">none ' +
        'connected</td></tr>';
      return;
    }
    reps.forEach(function (r) {
      var tr = document.createElement('tr');
      var stateCls = r.state === 'streaming' ? 'ok' :
        r.state === 'catching_up' ? 'info' : 'neutral';
      var lagCls = lagClass(r.behind_seconds, r.state === 'streaming');
      var cells = [
        ['replica', r.report_host ? esc(r.report_host) +
          '<span class="sub mono">' + esc(r.address) + '</span>' :
          '<span class="mono">' + esc(r.address) + '</span>'],
        ['version', esc(replicaVersion(r))],
        ['connected', '<span class="num">' + esc(time(r.since)) + '</span>' +
          '<span class="sub">' + esc(ago(r.since, d.now)) + ' ago</span>'],
        ['link', (r.tls ? badge('ok', 'TLS') : badge('warn', 'plain')) +
          (r.compression !== 'none' ? '<div class="stack">' +
            badge('neutral', r.compression) + '</div>' : '')],
        ['state', badge(stateCls, r.state.replace('_', ' '))],
        ['sent', '<span class="mono">' + esc(position(r.file, r.position)) +
          '</span><span class="sub">' + esc(time(r.timestamp)) + '</span>'],
        ['behind', '<span class="num">' + (r.behind_bytes == null ? '–' :
          esc(bytes(r.behind_bytes))) + '</span>', 'num'],
        ['lag', badge(lagCls, r.state === 'streaming' ? 'caught up' :
          r.behind_seconds == null ? 'unknown' : duration(r.behind_seconds)),
          'lag']
      ];
      cells.forEach(function (c) {
        var td = document.createElement('td');
        td.innerHTML = c[1];
        td.setAttribute('data-label', c[0]);
        if (c[2]) td.className = c[2];
        tr.appendChild(td);
      });
      rows.appendChild(tr);
    });
  }

  function render(d) {
    document.title = d.name + ' — ' + d.state;
    text('title', d.name);
    text('version', d.version);
    var st = $('state');
    st.className = 'state ' + stateClass(d.state);
    st.textContent = d.state;
    text('uptime', duration(d.uptime_seconds));
    text('now', time(d.now));
    renderSource(d);
    renderStorage(d);
    renderReplicas(d);
    $('error').style.display = 'none';
  }

  var failedSince = null, timer = null, lastOk = null;
  function fail(why) {
    if (failedSince === null) failedSince = new Date();
    var e = $('error');
    e.textContent = 'status unavailable since ' + time(failedSince / 1000) +
      ': ' + why + (lastOk ? ' — showing data from ' + time(lastOk / 1000)
      : '');
    e.style.display = 'block';
    var st = $('state');
    st.className = 'state bad';
    st.textContent = 'unreachable';
  }
  function schedule() {
    var ms = Number($('interval').value);
    if (timer) clearTimeout(timer);
    timer = ms ? setTimeout(poll, ms) : null;
    if (!ms) text('updated', 'paused' + (lastOk ? ', last update ' +
      time(lastOk / 1000) : ''));
  }
  function poll() {
    fetch('status.json', { cache: 'no-store' }).then(function (r) {
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return r.json();
    }).then(function (d) {
      failedSince = null;
      lastOk = new Date();
      render(d);
      text('updated', 'updated ' + time(lastOk / 1000));
    }).catch(function (err) {
      fail(err.message);
    }).then(schedule);
  }
  $('interval').addEventListener('change', function () {
    if (Number(this.value)) poll(); else schedule();
  });
  $('refresh').addEventListener('click', function () {
    if (timer) clearTimeout(timer);
    poll();
  });
  poll();
})();
