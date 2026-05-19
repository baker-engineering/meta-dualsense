// kbm-web frontend. Vanilla JS, no framework.
// Layouts: status, tuning sliders, bindings table, live controller test.

const ACTIONS = [
  'lstick_up','lstick_down','lstick_left','lstick_right',
  'rstick_up','rstick_down','rstick_left','rstick_right',
  'dpad_up','dpad_down','dpad_left','dpad_right',
  'cross','circle','square','triangle',
  'l1','r1','l2','r2','l3','r3',
  'create','options','ps','touchpad','mute',
];

// What each DualSense action is reported as on the Windows side after the
// bridge.ps1 translation to a virtual Xbox 360 controller. Shown in the UI so
// users can correlate their bindings with the Xbox-style prompts Warzone (and
// most PC games) show on screen.
const ACTION_XBOX = {
  cross:'A', circle:'B', square:'X', triangle:'Y',
  l1:'LB', r1:'RB', l2:'LT', r2:'RT', l3:'L3', r3:'R3',
  create:'Back', options:'Start', ps:'Guide',
  dpad_up:'D↑', dpad_down:'D↓', dpad_left:'D←', dpad_right:'D→',
  lstick_up:'LS↑', lstick_down:'LS↓', lstick_left:'LS←', lstick_right:'LS→',
  rstick_up:'RS↑', rstick_down:'RS↓', rstick_left:'RS←', rstick_right:'RS→',
  // touchpad/mute have no Xbox 360 equivalents in the bridge
};

function actionLabel(a) {
  const x = ACTION_XBOX[a];
  return x ? `${a}  (${x})` : a;
}

// HID-report byte offsets / bit masks (matches report.h on the device).
// Label includes the Xbox 360 button name in parentheses so the live preview
// lines up with the in-game prompts after the bridge's DS->X360 translation.
const BUTTONS = [
  // [label, byteIndex, bitMask]
  ['Square (X)',    8, 0x10],
  ['Cross (A)',     8, 0x20],
  ['Circle (B)',    8, 0x40],
  ['Triangle (Y)',  8, 0x80],
  ['L1 (LB)',       9, 0x01],
  ['R1 (RB)',       9, 0x02],
  ['L2 (LT)',       9, 0x04],
  ['R2 (RT)',       9, 0x08],
  ['Create (Back)', 9, 0x10],
  ['Options (Start)',9,0x20],
  ['L3',            9, 0x40],
  ['R3',            9, 0x80],
  ['PS (Guide)',   10, 0x01],
  ['Touch',        10, 0x02],
  ['Mute',         10, 0x04],
];

const DPAD = [null,'↑',null,'←',null,'→',null,'↓',null];
const DPAD_NAMES = ['N','NE','E','SE','S','SW','W','NW','none'];

let conf = null;

// ---- DOM helpers -----------------------------------------------------------

const $ = id => document.getElementById(id);
function text(id, v) { const e = $(id); if (e) e.textContent = (v === undefined || v === null || v === '') ? '—' : v; }
function setBadge(id, v) {
  const e = $(id);
  e.textContent = v || '—';
  e.classList.remove('active','failed','inactive','activating');
  if (v) e.classList.add(v);
}

// ---- Load status + config --------------------------------------------------

async function loadStatus() {
  try {
    const s = await fetch('/api/status').then(r => r.json());
    text('version', s.version);
    text('s-hostname', s.hostname);
    text('s-udc',      s.gadget_udc || s.gadget_udc === '' ? s.gadget_udc : '—');
    text('s-ustate',   s.udc_state);
    text('s-speed',    s.udc_speed);
    text('s-hidg',     s.hidg_present ? 'ready' : 'missing');
    text('s-rdesc',    s.report_desc_size + ' bytes' + (s.report_desc_size === 0 ? ' (placeholder — gadget will fail)' : ''));
    text('s-inputs',   s.input_devices && s.input_devices.length ? s.input_devices.join(', ') : '—');
    setBadge('s-gadget', s.gadget_service);
    setBadge('s-mapper', s.mapper_service);
    setModeUI(s.mode);
  } catch (e) {
    console.error('status:', e);
  }
}

function setModeUI(mode) {
  text('mode-current', 'current: ' + (mode || '—'));
  document.querySelectorAll('input[name="mode"]').forEach(r => {
    r.checked = (r.value === mode);
  });
}

document.querySelectorAll('input[name="mode"]').forEach(r => {
  r.addEventListener('change', async () => {
    if (!r.checked) return;
    const mode = r.value;
    if (mode === 'passthrough' && !confirm(
      'Switching to passthrough will disconnect the DualSense gadget. ' +
      'You may need to unplug + replug the USB cable to the host once. Continue?'
    )) {
      loadStatus();
      return;
    }
    const resp = await fetch('/api/mode', {
      method: 'POST',
      headers: {'Content-Type':'application/json'},
      body: JSON.stringify({mode}),
    });
    if (!resp.ok) { alert('Mode switch failed: ' + await resp.text()); return; }
    setTimeout(loadStatus, 1500);
  });
});

async function loadConfig() {
  try {
    conf = await fetch('/api/config').then(r => r.json());
    if (!conf.bursts) conf.bursts = [];
    if (!conf.hotkey) conf.hotkey = ['LEFTCTRL','ESC'];
    for (const k of ['window_ms','curve_exp','anti_deadzone','outer_sat','sens_counts_ms','debt_drain']) {
      const inp = document.querySelector(`input[name="${k}"]`);
      if (inp) { inp.value = conf.tuning[k]; inp.nextElementSibling.textContent = inp.value; }
    }
    populateRecoilUI();
    renderBindings();
    renderBursts();
    renderHotkey();
  } catch (e) {
    console.error('config:', e);
  }
}

// Populate the Recoil & sensitivity card from conf.tuning. Action dropdowns
// pull options from the same ACTIONS list the binding picker uses, with the
// Xbox glyph in parentheses for orientation.
function populateRecoilUI() {
  for (const id of ['recoil-action','ads-action']) {
    const sel = document.getElementById(id);
    if (sel.options.length <= 1) {
      for (const a of ACTIONS) {
        const o = document.createElement('option');
        o.value = a;
        o.textContent = actionLabel(a);
        sel.appendChild(o);
      }
    }
  }
  document.getElementById('recoil-action').value = conf.tuning.recoil_action || '';
  document.getElementById('ads-action').value    = conf.tuning.ads_action    || '';
  const sliders = {
    recoil_x:        conf.tuning.recoil_x        ?? 0,
    recoil_y:        conf.tuning.recoil_y        ?? 0,
    fire_sens_scale: conf.tuning.fire_sens_scale ?? 1.0,
    ads_sens_scale:  conf.tuning.ads_sens_scale  ?? 1.0,
  };
  for (const [k, v] of Object.entries(sliders)) {
    const inp = document.querySelector(`input[name="${k}"]`);
    if (inp) { inp.value = v; inp.nextElementSibling.textContent = inp.value; }
  }
}

document.querySelectorAll('input[type="range"]').forEach(inp => {
  inp.addEventListener('input', () => { inp.nextElementSibling.textContent = inp.value; });
});

$('tuning-form').addEventListener('submit', async e => {
  e.preventDefault();
  if (!conf) return;
  for (const k of ['window_ms','curve_exp','anti_deadzone','outer_sat','sens_counts_ms','debt_drain']) {
    const inp = document.querySelector(`input[name="${k}"]`);
    conf.tuning[k] = parseFloat(inp.value);
  }
  await postConfig();
});

$('btn-reset').addEventListener('click', () => {
  if (!conf) return;
  Object.assign(conf.tuning, { window_ms: 6, curve_exp: 2.0, anti_deadzone: 0.10, outer_sat: 0.97, sens_counts_ms: 8.0, debt_drain: 0.05 });
  for (const k of ['window_ms','curve_exp','anti_deadzone','outer_sat','sens_counts_ms','debt_drain']) {
    const inp = document.querySelector(`input[name="${k}"]`);
    if (inp) { inp.value = conf.tuning[k]; inp.nextElementSibling.textContent = inp.value; }
  }
});

$('recoil-form').addEventListener('submit', async e => {
  e.preventDefault();
  if (!conf) return;
  conf.tuning.recoil_action    = document.getElementById('recoil-action').value;
  conf.tuning.ads_action       = document.getElementById('ads-action').value;
  conf.tuning.recoil_x         = parseFloat(document.querySelector('input[name="recoil_x"]').value);
  conf.tuning.recoil_y         = parseFloat(document.querySelector('input[name="recoil_y"]').value);
  conf.tuning.fire_sens_scale  = parseFloat(document.querySelector('input[name="fire_sens_scale"]').value);
  conf.tuning.ads_sens_scale   = parseFloat(document.querySelector('input[name="ads_sens_scale"]').value);
  await postConfig();
});

$('btn-recoil-reset').addEventListener('click', () => {
  if (!conf) return;
  Object.assign(conf.tuning, {
    recoil_action: '', recoil_x: 0, recoil_y: 0,
    ads_action: '', ads_sens_scale: 1.0, fire_sens_scale: 1.0,
  });
  populateRecoilUI();
});

$('btn-reload').addEventListener('click', () => { loadStatus(); loadConfig(); });
document.querySelectorAll('[data-restart]').forEach(b => {
  b.addEventListener('click', () => restart(b.dataset.restart));
});

// ---- Bindings UI -----------------------------------------------------------

function renderBindings() {
  const tb = document.querySelector('#bindings tbody');
  tb.innerHTML = '';
  for (let i = 0; i < conf.bindings.length; i++) {
    const b = conf.bindings[i];
    const tr = document.createElement('tr');
    tr.innerHTML = `<td>${b.source}</td><td>${b.code}</td><td>${actionLabel(b.action)}</td>` +
                   `<td style="text-align:right"><button data-i="${i}">×</button></td>`;
    tr.querySelector('button').addEventListener('click', () => { conf.bindings.splice(i,1); renderBindings(); });
    tb.appendChild(tr);
  }
  const ac = $('b-action');
  if (!ac.options.length) ac.innerHTML = ACTIONS.map(a => `<option value="${a}">${actionLabel(a)}</option>`).join('');
}

$('btn-add-binding').addEventListener('click', () => {
  if (!conf) return;
  const source = $('b-source').value;
  const code = $('b-code').value.trim().toUpperCase();
  const action = $('b-action').value;
  if (!code) return;
  // Replace existing binding for the same source+code.
  const i = conf.bindings.findIndex(b => b.source === source && b.code === code);
  if (i >= 0) conf.bindings[i].action = action;
  else conf.bindings.push({source, code, action});
  $('b-code').value = '';
  renderBindings();
});

$('btn-save-bindings').addEventListener('click', postConfig);

// ---- Burst-on-hold ---------------------------------------------------------

function renderBursts() {
  const tb = document.querySelector('#bursts tbody');
  tb.innerHTML = '';
  for (let i = 0; i < conf.bursts.length; i++) {
    const br = conf.bursts[i];
    const tr = document.createElement('tr');
    tr.innerHTML = `<td>${actionLabel(br.action)}</td>` +
                   `<td>${br.hz}</td>` +
                   `<td>${br.duty || 0.5}</td>` +
                   `<td>${br.jitter || 0}</td>` +
                   `<td>${br.duty_jitter || 0}</td>` +
                   `<td>${br.skip_prob || 0}</td>` +
                   `<td style="text-align:right"><button data-i="${i}">×</button></td>`;
    tr.querySelector('button').addEventListener('click', () => { conf.bursts.splice(i,1); renderBursts(); });
    tb.appendChild(tr);
  }
  const sel = $('burst-action');
  if (!sel.options.length) sel.innerHTML = ACTIONS.map(a => `<option value="${a}">${actionLabel(a)}</option>`).join('');
}

// clamp01 returns v clamped to [0, 1] or 0 if v is NaN/empty.
function clamp01(v) {
  if (isNaN(v) || v < 0) return 0;
  if (v > 1) return 1;
  return v;
}

$('btn-add-burst').addEventListener('click', () => {
  if (!conf) return;
  const action     = $('burst-action').value;
  const hz         = parseFloat($('burst-hz').value);
  const duty       = parseFloat($('burst-duty').value);
  const jitter     = parseFloat($('burst-jitter').value);
  const dutyJitter = parseFloat($('burst-duty-jitter').value);
  const skipProb   = parseFloat($('burst-skip-prob').value);
  if (!action || !(hz > 0)) { alert('Pick an action and an hz > 0.'); return; }
  const i = conf.bursts.findIndex(b => b.action === action);
  const entry = {
    action, hz,
    duty:        isNaN(duty) || duty <= 0 ? 0.5 : Math.min(1, duty),
    jitter:      clamp01(jitter),
    duty_jitter: clamp01(dutyJitter),
    skip_prob:   clamp01(skipProb),
  };
  if (i >= 0) conf.bursts[i] = entry;
  else        conf.bursts.push(entry);
  $('burst-hz').value          = '';
  $('burst-duty').value        = '';
  $('burst-jitter').value      = '';
  $('burst-duty-jitter').value = '';
  $('burst-skip-prob').value   = '';
  renderBursts();
});

$('btn-save-bursts').addEventListener('click', postConfig);

async function postConfig() {
  const r = await fetch('/api/config', {
    method: 'POST',
    headers: {'Content-Type':'application/json'},
    body: JSON.stringify(conf),
  });
  if (!r.ok) { alert('Save failed: ' + await r.text()); return; }
  setTimeout(() => { loadStatus(); loadConfig(); }, 600);
}

async function restart(unit) {
  const r = await fetch('/api/restart?unit=' + encodeURIComponent(unit), {method: 'POST'});
  if (!r.ok) { alert('Restart failed'); return; }
  setTimeout(loadStatus, 800);
}

// ---- Controller test (SSE) -------------------------------------------------

function ensureButtonLEDs() {
  const root = $('buttons');
  if (root.children.length) return;
  for (const [name] of BUTTONS) {
    const d = document.createElement('div');
    d.className = 'btn-led';
    d.dataset.name = name;
    d.textContent = name;
    root.appendChild(d);
  }
  const dp = $('dpad');
  for (let i = 0; i < 9; i++) {
    const d = document.createElement('div');
    d.textContent = DPAD[i] || '';
    d.dataset.idx = i;
    dp.appendChild(d);
  }
}

function startSSE() {
  const ev = new EventSource('/api/state');
  ev.addEventListener('message', e => {
    if (!e.data) return;
    const bytes = hexToBytes(e.data);
    drawState(bytes);
    setConn(true);
  });
  ev.addEventListener('stale', () => setConn(false));
  ev.onerror = () => setConn(false);
}

function setConn(ok) {
  $('conn-dot').classList.toggle('stale', !ok);
  $('conn-text').textContent = ok ? 'live' : 'no live data';
}

function hexToBytes(h) {
  const out = new Uint8Array(h.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(h.substr(i*2, 2), 16);
  return out;
}

function drawState(b) {
  if (b.length < 11) return;
  // Sticks
  const LX = b[1], LY = b[2], RX = b[3], RY = b[4];
  drawStick('lstick', LX, LY);
  drawStick('rstick', RX, RY);
  text('LXv', LX); text('LYv', LY); text('RXv', RX); text('RYv', RY);
  // Triggers
  const L2 = b[5], R2 = b[6];
  $('L2bar').style.width = (L2 / 2.55) + '%';
  $('R2bar').style.width = (R2 / 2.55) + '%';
  text('L2v', L2); text('R2v', R2);
  // Buttons
  for (const [name, byteIdx, mask] of BUTTONS) {
    const led = document.querySelector(`.btn-led[data-name="${name}"]`);
    if (led) led.classList.toggle('on', (b[byteIdx] & mask) !== 0);
  }
  // D-pad (nibble in byte 8 low 4 bits, value 0-7 or 8=null)
  const hat = b[8] & 0x0F;
  document.querySelectorAll('#dpad > div').forEach(d => d.classList.remove('on'));
  const map = {0: 1, 1: 2, 2: 5, 3: 8, 4: 7, 5: 6, 6: 3, 7: 0};  // hat → grid index
  if (hat in map) {
    const cell = document.querySelector(`#dpad > div[data-idx="${map[hat]}"]`);
    if (cell) cell.classList.add('on');
  }
}

function drawStick(id, xByte, yByte) {
  const c = $(id);
  const ctx = c.getContext('2d');
  const w = c.width, h = c.height;
  ctx.clearRect(0, 0, w, h);
  // Outer ring
  ctx.fillStyle = '#fff';
  ctx.strokeStyle = '#e5e5ea';
  ctx.lineWidth = 1;
  ctx.beginPath(); ctx.arc(w/2, h/2, w/2 - 3, 0, 2*Math.PI); ctx.fill(); ctx.stroke();
  // Cross hairs
  ctx.beginPath(); ctx.moveTo(w/2, 8); ctx.lineTo(w/2, h-8); ctx.stroke();
  ctx.beginPath(); ctx.moveTo(8, h/2); ctx.lineTo(w-8, h/2); ctx.stroke();
  // Center dot
  ctx.fillStyle = '#c7c7cc';
  ctx.beginPath(); ctx.arc(w/2, h/2, 3, 0, 2*Math.PI); ctx.fill();
  // Stick position
  const dx = (xByte - 128) / 127;
  const dy = (yByte - 128) / 127;
  const r = w/2 - 14;
  const px = w/2 + dx * r;
  const py = h/2 + dy * r;
  // Stick base trail
  ctx.strokeStyle = '#0a84ff';
  ctx.lineWidth = 2;
  ctx.beginPath(); ctx.moveTo(w/2, h/2); ctx.lineTo(px, py); ctx.stroke();
  // Stick knob
  const grd = ctx.createRadialGradient(px, py, 0, px, py, 12);
  grd.addColorStop(0, '#5ac8fa');
  grd.addColorStop(1, '#0a84ff');
  ctx.fillStyle = grd;
  ctx.beginPath(); ctx.arc(px, py, 10, 0, 2*Math.PI); ctx.fill();
}

// ---- Profiles --------------------------------------------------------------

const KEY_NAME_BY_CODE = (() => {
  // Common Linux KEY_* / BTN_* names indexed by event-code. Kept in sync with
  // the daemon's config_*_from_name lookups so the captured value matches what
  // /etc/kbm-mapper.conf accepts. Falls back to the numeric code if unmapped.
  const m = new Map();
  const KEYS = {
    1:'ESC',
    2:'1',3:'2',4:'3',5:'4',6:'5',7:'6',8:'7',9:'8',10:'9',11:'0',
    14:'BACKSPACE',15:'TAB',
    16:'Q',17:'W',18:'E',19:'R',20:'T',21:'Y',22:'U',23:'I',24:'O',25:'P',
    28:'ENTER',29:'LEFTCTRL',
    30:'A',31:'S',32:'D',33:'F',34:'G',35:'H',36:'J',37:'K',38:'L',
    41:'GRAVE',42:'LEFTSHIFT',
    44:'Z',45:'X',46:'C',47:'V',48:'B',49:'N',50:'M',
    54:'RIGHTSHIFT',56:'LEFTALT',57:'SPACE',
    59:'F1',60:'F2',61:'F3',62:'F4',63:'F5',64:'F6',65:'F7',66:'F8',67:'F9',68:'F10',
    87:'F11',88:'F12',
    97:'RIGHTCTRL',100:'RIGHTALT',
    103:'UP',105:'LEFT',106:'RIGHT',108:'DOWN',
  };
  for (const k in KEYS) m.set(parseInt(k,10), {source:'key', code:KEYS[k]});
  // BTN_* are in the 0x110+ (272+) range
  m.set(0x110, {source:'mouse', code:'LEFT'});    // BTN_LEFT
  m.set(0x111, {source:'mouse', code:'RIGHT'});   // BTN_RIGHT
  m.set(0x112, {source:'mouse', code:'MIDDLE'});  // BTN_MIDDLE
  m.set(0x113, {source:'mouse', code:'SIDE'});    // BTN_SIDE
  m.set(0x114, {source:'mouse', code:'EXTRA'});   // BTN_EXTRA
  return m;
})();

let profiles = [];
let captureES = null;

function fmtTime(s) {
  if (!s) return '';
  const d = new Date(s);
  if (isNaN(d.getTime())) return '';
  return d.toLocaleString();
}

async function loadProfiles() {
  try {
    profiles = await fetch('/api/profiles').then(r => r.json());
    renderProfiles();
  } catch (e) { console.error('profiles:', e); }
}

function renderProfiles() {
  const tb = document.querySelector('#profiles tbody');
  tb.innerHTML = '';
  for (const p of profiles) {
    const tr = document.createElement('tr');
    if (p.active) tr.classList.add('active');
    tr.innerHTML =
      `<td>${p.active ? '● ' : ''}${escapeHtml(p.name)}</td>` +
      `<td>${escapeHtml(p.description || '')}</td>` +
      `<td>${fmtTime(p.modified)}</td>` +
      `<td style="text-align:right; white-space:nowrap">` +
        (p.active ? '' : `<button class="btn ghost" data-act="activate">Activate</button> `) +
        `<button class="btn ghost" data-act="rename">Rename</button> ` +
        `<a class="btn ghost" href="/api/profiles/${encodeURIComponent(p.id)}/export">Export</a> ` +
        (p.active ? '' : `<button class="btn ghost" data-act="delete">Delete</button>`) +
      `</td>`;
    tr.querySelectorAll('button[data-act]').forEach(b => {
      b.addEventListener('click', () => profileAction(b.dataset.act, p));
    });
    tb.appendChild(tr);
  }
}

function escapeHtml(s) {
  return String(s||'').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
}

async function profileAction(act, p) {
  if (act === 'activate') {
    const r = await fetch('/api/profiles/'+encodeURIComponent(p.id)+'/activate', {method:'POST'});
    if (!r.ok) { alert('activate failed: ' + await r.text()); return; }
    await loadProfiles();
    await loadConfig();
  } else if (act === 'rename') {
    const name = prompt('New name:', p.name);
    if (name === null) return;
    const desc = prompt('Description:', p.description || '') || '';
    const r = await fetch('/api/profiles/'+encodeURIComponent(p.id), {
      method: 'PUT',
      headers: {'Content-Type':'application/json'},
      body: JSON.stringify({name, description: desc})
    });
    if (!r.ok) { alert('rename failed: ' + await r.text()); return; }
    await loadProfiles();
  } else if (act === 'delete') {
    if (!confirm(`Delete profile "${p.name}"?`)) return;
    const r = await fetch('/api/profiles/'+encodeURIComponent(p.id), {method:'DELETE'});
    if (!r.ok) { alert('delete failed: ' + await r.text()); return; }
    await loadProfiles();
  }
}

$('btn-prof-new').addEventListener('click', async () => {
  const name = $('prof-new-name').value.trim();
  if (!name) { alert('Name is required'); return; }
  const desc = $('prof-new-desc').value.trim();
  const active = profiles.find(p => p.active);
  const r = await fetch('/api/profiles', {
    method: 'POST',
    headers: {'Content-Type':'application/json'},
    body: JSON.stringify({name, description: desc, copy_from: active ? active.id : ''})
  });
  if (!r.ok) { alert('create failed: ' + await r.text()); return; }
  $('prof-new-name').value = '';
  $('prof-new-desc').value = '';
  await loadProfiles();
});

$('prof-import-file').addEventListener('change', async (e) => {
  const f = e.target.files[0];
  if (!f) return;
  const body = await f.arrayBuffer();
  const r = await fetch('/api/profiles/import', {
    method: 'POST',
    headers: {'Content-Type':'text/plain'},
    body
  });
  if (!r.ok) { alert('import failed: ' + await r.text()); return; }
  e.target.value = '';
  await loadProfiles();
});

// ---- Capture ---------------------------------------------------------------

$('btn-capture').addEventListener('click', () => {
  if (captureES) { captureCancel(); return; }
  captureStart();
});

function captureStart() {
  const status = $('capture-status');
  status.hidden = false;
  status.textContent = 'Press a key or click a mouse button on the BBB-attached device…';
  $('btn-capture').textContent = 'Cancel';
  captureES = new EventSource('/api/capture');
  captureES.addEventListener('message', (ev) => {
    try {
      const e = JSON.parse(ev.data);
      // Only key-down events (value === 1) so we ignore the release after the
      // user lifts the key.
      if (e.value !== 1) return;
      const mapped = KEY_NAME_BY_CODE.get(e.code);
      if (mapped) {
        $('b-source').value = mapped.source;
        $('b-code').value = mapped.code;
        status.textContent = `Captured: ${mapped.source} ${mapped.code} (code ${e.code})`;
      } else {
        status.textContent = `Captured raw code ${e.code} from ${e.source} — no symbolic name; type it manually`;
      }
      captureCancel(false);
    } catch {}
  });
  captureES.addEventListener('unavailable', (ev) => {
    status.textContent = `Capture unavailable: ${ev.data}. Switch to emulation mode first.`;
    captureCancel(false);
  });
  captureES.addEventListener('error', () => {
    // Silent: kbm-web closes the stream on success or when daemon socket drops.
  });
}

function captureCancel(resetStatus = true) {
  if (captureES) { captureES.close(); captureES = null; }
  $('btn-capture').textContent = 'Capture';
  if (resetStatus) { $('capture-status').hidden = true; $('capture-status').textContent = ''; }
}

// ---- Mode-toggle hotkey ----------------------------------------------------

let hotkeyCaptureES = null;
let hotkeyDraft = null;  // pending chord while capture is in progress

function renderHotkey() {
  const cur = (conf && conf.hotkey && conf.hotkey.length)
    ? conf.hotkey.join(' + ')
    : '(disabled)';
  $('hotkey-current').textContent = cur;
}

function hotkeyKeyName(code) {
  // Match exactly what daemon's name-lookup recognizes for hotkey.mode_toggle.
  // KEY_NAME_BY_CODE is keyed for binding capture; for hotkey we want the
  // SYMBOLIC name even for modifier keys (binding-capture skips modifiers).
  const M = {
    [0x1d]:'LEFTCTRL',  [0x61]:'RIGHTCTRL',
    [0x2a]:'LEFTSHIFT', [0x36]:'RIGHTSHIFT',
    [0x38]:'LEFTALT',   [0x64]:'RIGHTALT',
    [0x7d]:'LEFTMETA',  [0x7e]:'RIGHTMETA',
    [0x01]:'ESC', [0x0f]:'TAB', [0x39]:'SPACE', [0x1c]:'ENTER',
    [0x0e]:'BACKSPACE', [0x29]:'GRAVE',
    [0x67]:'UP', [0x6c]:'DOWN', [0x69]:'LEFT', [0x6a]:'RIGHT',
    [0x3b]:'F1',[0x3c]:'F2',[0x3d]:'F3',[0x3e]:'F4',[0x3f]:'F5',[0x40]:'F6',
    [0x41]:'F7',[0x42]:'F8',[0x43]:'F9',[0x44]:'F10',[0x57]:'F11',[0x58]:'F12',
  };
  if (M[code]) return M[code];
  // Letters/digits via KEY_NAME_BY_CODE
  const mapped = KEY_NAME_BY_CODE.get(code);
  if (mapped && mapped.source === 'key') return mapped.code;
  return null;
}

$('btn-hotkey-capture').addEventListener('click', () => {
  if (hotkeyCaptureES) { hotkeyCaptureCancel(); return; }
  hotkeyCaptureStart();
});

$('btn-hotkey-save').addEventListener('click', async () => {
  if (!hotkeyDraft || hotkeyDraft.length === 0) return;
  conf.hotkey = hotkeyDraft;
  await postConfig();
  hotkeyDraft = null;
  $('btn-hotkey-save').disabled = true;
  $('hotkey-status').textContent = 'Saved — daemon hot-reloaded.';
  setTimeout(() => { $('hotkey-status').textContent = ''; }, 3000);
});

function hotkeyCaptureStart() {
  $('btn-hotkey-capture').textContent = 'Cancel';
  $('hotkey-status').textContent = 'Hold the keys you want, release when done…';
  const pressed = new Set();
  const order = [];
  hotkeyCaptureES = new EventSource('/api/capture');
  hotkeyCaptureES.addEventListener('message', (ev) => {
    try {
      const e = JSON.parse(ev.data);
      const name = hotkeyKeyName(e.code);
      if (!name) return;
      if (e.value === 1) {
        if (!pressed.has(name)) { pressed.add(name); order.push(name); }
        $('hotkey-current').textContent = order.join(' + ') + ' …';
      } else if (e.value === 0) {
        // First release after at least one key was pressed -> finalize.
        if (order.length > 0) {
          hotkeyDraft = order.slice();
          $('hotkey-current').textContent = hotkeyDraft.join(' + ') + ' (unsaved)';
          $('btn-hotkey-save').disabled = false;
          hotkeyCaptureCancel(false);
        }
      }
    } catch {}
  });
  hotkeyCaptureES.addEventListener('unavailable', (ev) => {
    $('hotkey-status').textContent = `Capture unavailable: ${ev.data}. Switch to emulation mode to use the hotkey capture.`;
    hotkeyCaptureCancel(false);
  });
}

function hotkeyCaptureCancel(resetStatus = true) {
  if (hotkeyCaptureES) { hotkeyCaptureES.close(); hotkeyCaptureES = null; }
  $('btn-hotkey-capture').textContent = 'Capture chord';
  if (resetStatus) { $('hotkey-status').textContent = ''; renderHotkey(); }
}

// ---- Init ------------------------------------------------------------------

ensureButtonLEDs();
loadStatus();
loadConfig();
loadProfiles();
startSSE();
// Refresh status periodically so service-state badges stay current.
setInterval(loadStatus, 4000);
