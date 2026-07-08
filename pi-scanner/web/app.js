"use strict";
// Careflow RS-485 Scanner — kiosk wizard. Zero framework: /ws pushes state/log/candidate/payload,
// the UI renders per state and posts operator edits back (debounced) to keep the payload meter live.

const $ = (sel, root = document) => root.querySelector(sel);
const api = (path, body) =>
  fetch("/api" + path, body ? { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) } : {})
    .then((r) => r.json().catch(() => ({})));

let session = { state: "idle", candidate: null, payload: null, operator: {} };
let lastRenderedState = null;
let debounce = null;

// --- WebSocket ---------------------------------------------------------------------------------
function connect() {
  const ws = new WebSocket(`ws://${location.host}/ws`);
  ws.onmessage = (ev) => {
    const msg = JSON.parse(ev.data);
    if (msg.type === "snapshot") { session = msg.data; render(true); }
    else if (msg.type === "state") { session.state = msg.data; render(); }
    else if (msg.type === "log") { appendLog(msg.data); }
    else if (msg.type === "candidate") { session.candidate = msg.data; if (session.state === "preparing_profile") render(); }
    else if (msg.type === "error") { appendLog("ERROR: " + msg.data); }
  };
  ws.onclose = () => setTimeout(connect, 1500);
}

function appendLog(line) {
  const el = $("#log");
  el.textContent += (el.textContent ? "\n" : "") + line;
  el.scrollTop = el.scrollHeight;
}

// --- render ------------------------------------------------------------------------------------
const BUSY = { searching: "Searching for the device…", flashing: "Flashing the node…", probing: "Provisioning + joining…", profiling: "Installing decoder + verifying uplink…" };

function render(forceLogs) {
  const st = session.state;
  const banner = $("#stateBanner");
  banner.textContent = st.replace(/_/g, " ");
  banner.className = "state state-" + st;
  if (forceLogs && session.logs) { $("#log").textContent = session.logs.join("\n"); $("#log").scrollTop = 1e9; }
  if (st === lastRenderedState && st !== "preparing_profile") return;
  lastRenderedState = st;

  const panel = $("#panel");
  panel.innerHTML = "";
  if (st === "idle") return renderIdle(panel);
  if (st === "preparing_profile") return renderPrepare(panel);
  if (st === "success" || st === "failed") return renderDone(panel);
  return renderBusy(panel); // searching / flashing / probing / profiling
}

function tpl(id) { return document.importNode($("#" + id).content, true); }

function renderIdle(panel) {
  panel.appendChild(tpl("tpl-idle"));
  api("/ports").then((d) => {
    const sel = $("#port");
    (d.ports || ["mock"]).forEach((p) => { const o = document.createElement("option"); o.value = o.textContent = p; sel.appendChild(o); });
  });
  $("#btnStart").onclick = () => api("/session/start", { port: $("#port").value });
}

function renderBusy(panel) {
  panel.appendChild(tpl("tpl-busy"));
  $("#busyTitle").textContent = BUSY[session.state] || "Working…";
}

function renderPrepare(panel) {
  panel.appendChild(tpl("tpl-prepare"));
  // metadata form
  const meta = $("#meta");
  Object.entries(session.operator || {}).forEach(([k, v]) => { if (meta[k] != null && v != null) meta[k].value = v; });
  meta.oninput = pushEdits;

  // candidate table
  const tb = $("#cand tbody");
  const cand = session.candidate || { measurands: [] };
  cand.measurands.forEach((m, i) => tb.appendChild(row(m, i)));
  tb.oninput = pushEdits;

  $("#btnConfirm").onclick = () => api("/session/confirm", {}); // {} -> POST (api() GETs when bodyless)
  $("#btnReset").onclick = () => api("/session/reset", {});
  updateMeter(session.payload);
}

function row(m, i) {
  const tr = document.createElement("tr");
  tr.dataset.fc = m.fc; tr.dataset.addr = m.addr;
  const sample = m.sample_value == null ? "" : m.sample_value;
  tr.innerHTML = `
    <td><input type="checkbox" class="inc" ${m.include_in_payload ? "checked" : ""}></td>
    <td>${m.fc}</td><td>${m.addr}</td>
    <td>${sel("dtype", ["float32","u16","i16","u32","i32"], m.dtype)}</td>
    <td>${sel("word", ["ABCD","CDAB","BADC","DCBA"], m.word_order)}</td>
    <td>${sample}</td>
    <td class="conf">${(m.confidence * 100 | 0)}%</td>
    <td><input class="key" data-f="key" placeholder="e.g. v1n" value="${m.key || ""}"></td>
    <td><input data-f="name" value="${m.name || ""}"></td>
    <td><input data-f="unit" style="min-width:60px" value="${m.unit || ""}"></td>
    <td><input data-f="scale" style="min-width:60px" value="${m.scale ?? 1}"></td>`;
  return tr;
}
function sel(cls, opts, val) {
  return `<select class="${cls}">` + opts.map((o) => `<option ${o === val ? "selected" : ""}>${o}</option>`).join("") + "</select>";
}

function gatherEdits() {
  return [...$("#cand tbody").querySelectorAll("tr")].map((tr) => ({
    fc: +tr.dataset.fc, addr: +tr.dataset.addr,
    include_in_payload: tr.querySelector(".inc").checked,
    dtype: tr.querySelector(".dtype").value,
    word_order: tr.querySelector(".word").value,
    key: tr.querySelector('[data-f="key"]').value.trim(),
    name: tr.querySelector('[data-f="name"]').value,
    unit: tr.querySelector('[data-f="unit"]').value,
    scale: parseFloat(tr.querySelector('[data-f="scale"]').value) || 1,
  }));
}

function pushEdits() {
  clearTimeout(debounce);
  debounce = setTimeout(() => {
    const meta = $("#meta");
    api("/session/operator-input", {
      manufacturer: meta.manufacturer.value, model: meta.model.value, category: meta.category.value,
      label: meta.label.value, notes: meta.notes.value, measurand_edits: gatherEdits(),
    }).then((r) => { session.payload = r.payload; updateMeter(r.payload); });
  }, 250);
}

function updateMeter(p) {
  if (!p) return;
  const pct = Math.min(100, (p.requested / p.budget) * 100);
  const fill = $("#meterFill");
  if (fill) { fill.style.width = pct + "%"; fill.classList.toggle("over", p.over_budget); }
  const txt = $("#meterText"); if (txt) txt.textContent = `payload ${p.requested} / ${p.budget} B${p.over_budget ? " — OVER BUDGET, deselect rows" : ""}`;
  const btn = $("#btnConfirm"); if (btn) btn.disabled = p.over_budget || p.fields === 0;
}

function renderDone(panel) {
  panel.appendChild(tpl("tpl-done"));
  const ok = session.state === "success";
  $("#doneTitle").textContent = ok ? "✅ Onboarded" : "❌ Failed";
  const s = $("#doneSummary");
  if (ok && session.artifacts) {
    s.innerHTML = `<p>Profile <strong>${session.artifacts.profile_id}</strong> is now in the Careflow pipeline.</p>
      <code>${session.artifacts.profile_path}</code><code>blob: ${(session.artifacts.blob_hex||"").slice(0,48)}…</code>`;
  } else {
    s.innerHTML = `<p class="muted">${session.error || "See the log for details."}</p>`;
  }
  $("#btnEdit").onclick = () => { session.state = "preparing_profile"; render(); };
  $("#btnRetry").onclick = () => api("/session/retry", {});
  $("#btnReset2").onclick = () => api("/session/reset", {});
}

// Draw the UI FIRST so nothing below can keep the app from rendering.
connect();
api("/session").then((s) => { session = s; render(true); });

// --- kiosk touch controls (no physical keyboard) ---------------------------------------------
// Show the on-screen keyboard when a text field is focused, hide it otherwise; a manual ⌨ toggle;
// and a ⏻ Exit with an in-page (non-modal) confirm — Chromium suppresses window.confirm() in the
// kiosk (it returns false), so the old confirm()-gated Exit never fired. Buttons are bound via
// pointerup+click so a real finger tap always registers, not just a synthesized click. Wrapped so a
// kiosk-only failure can never blank the wizard. (The keyboard itself is squeekboard, driven by the
// backend over D-Bus; this side just posts show/hide/toggle to /api/kiosk/keyboard.)
try {
  // Fire fn once per tap. pointerup is reliable for touch (no 300ms delay, no click-synthesis
  // dependency); we swallow the click the browser then synthesizes so the handler runs exactly once.
  const onTap = (el, fn) => {
    if (!el) return;
    let viaPointer = false;
    el.addEventListener("pointerup", (e) => {
      if (e.button > 0) return; // primary/touch only
      viaPointer = true; setTimeout(() => (viaPointer = false), 700);
      fn(e);
    });
    el.addEventListener("click", (e) => { if (!viaPointer) fn(e); });
  };

  // The backend reports the keyboard's real visibility as {shown}, so we stay in sync instead of
  // trusting a local flag that drifts (e.g. Chromium auto-focusing a field on load).
  let oskShown = false;
  const syncOsk = (r) => { if (r && typeof r.shown === "boolean") oskShown = r.shown; };
  const osk = (show) => { if (show === oskShown) return; oskShown = show; api("/kiosk/keyboard", { show }).then(syncOsk); };
  document.addEventListener("focusin", (e) => { if (e.target.matches("input,select,textarea")) osk(true); });
  document.addEventListener("focusout", () => setTimeout(() => {
    if (!document.querySelector("input:focus,select:focus,textarea:focus")) osk(false);
  }, 200));

  // ⌨ manual toggle — the backend flips the keyboard based on its real visibility, so this works
  // regardless of the local flag; we adopt the real state it reports back.
  onTap($("#kbToggle"), () => api("/kiosk/keyboard", { toggle: true }).then(syncOsk));

  // ⏻ Exit — two-tap confirm (no modal): first tap arms + relabels, second tap within 3s exits.
  // NOTE: api() sends GET unless a body is passed; /api/kiosk/exit is POST-only, so pass {} → POST.
  const ex = $("#kioskExit");
  if (ex) {
    const exLabel = ex.textContent;
    let armed = null;
    const disarm = () => { armed = null; ex.textContent = exLabel; ex.classList.remove("armed"); };
    onTap(ex, () => {
      if (armed) { clearTimeout(armed); disarm(); api("/kiosk/exit", {}); return; }
      ex.textContent = "⏻ Tap again"; ex.classList.add("armed");
      armed = setTimeout(disarm, 3000);
    });
  }
} catch (e) { console.error("kiosk controls init failed", e); }
