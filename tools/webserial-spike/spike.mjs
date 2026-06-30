/**
 * spike.mjs — Phase 7 WebSerial flash + NVS console round-trip prototype.
 *
 * SPIKE / VALIDATION ONLY — throwaway. Production flasher: plan 02-08.
 * Validates assumptions A1-A5 from 02-RESEARCH.md § "Assumptions Log".
 *
 * Two-phase flow (from 02-RESEARCH.md § "WebSerial + esptool-js Implementation Guide"):
 *   Phase 1: Transport + ESPLoader.main() → read EUI-48 MAC → writeFlash
 *             (eraseAll:false, never true — T-02-01) → hard_reset → disconnect
 *   Phase 2: port.open(@115200) → wait for "esp> " → send prov cmds
 *             → reboot → read boot log for [creds:NVS] / PSRAM / modbus
 *
 * Threat model (02-01-PLAN.md):
 *   T-02-01: eraseAll:false enforced below — search "NEVER" comments.
 *   T-02-02: appKey NEVER passed to appendLog() or any string concatenation.
 *   T-02-03: binarySha256 verified before flash.
 *   T-02-04: chip MAC confirmed before any write.
 *
 * esptool-js imported from CDN (CDN = jsdelivr, pinned to 0.6.0 — no postinstall;
 * Espressif-maintained Apache-2.0; Package Legitimacy Audit: APPROVED in 02-RESEARCH.md).
 *
 * UNRESOLVED assumptions (require hardware to resolve — recorded per RUNBOOK A2-A5):
 *   A2: exact readMac() call in esptool-js 0.6.0 — marked [A2-PROBE] below.
 *   A3: line terminator \r\n vs \n — marked [A3-PROBE] below.
 *   A4: reboot command vs RTS pulse — marked [A4-PROBE] below.
 *   A5: port re-enumeration after hard_reset on macOS/Chrome — marked [A5-PROBE] below.
 */

// ---------------------------------------------------------------------------
// esptool-js 0.6.0 via CDN — pinned version, Espressif-official npm package.
// ---------------------------------------------------------------------------
import { ESPLoader, Transport } from 'https://cdn.jsdelivr.net/npm/esptool-js@0.6.0/+esm';

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

/** Retained SerialPort object — needed for Phase 2 re-open after Phase 1. */
let g_port = null;
/** Derived DevEUI from chip MAC — set after Phase 1 MAC read. */
let g_deveui = null;
/** Chip MAC (EUI-48, colon-hex) — set after Phase 1, confirmed before flash. */
let g_mac = null;
/** binaryUrl from firmware service — set after fetchProtocol(). */
let g_binary_url = null;
/** binarySha256 from firmware service — for T-02-03 integrity check. */
let g_binary_sha256 = null;
/** ESPLoader instance — retained between confirmAndFlash and later use. */
let g_esploader = null;
/** Transport instance — retained for disconnect after flash. */
let g_transport = null;

// ---------------------------------------------------------------------------
// Logging helpers
// ---------------------------------------------------------------------------

function appendLog(msg, cls = '') {
  const log = document.getElementById('log');
  const line = document.createElement('span');
  if (cls) line.className = cls;
  line.textContent = msg + '\n';
  log.appendChild(line);
  log.scrollTop = log.scrollHeight;
}
function logOK(m)   { appendLog('[OK] ' + m, 'log-ok'); }
function logWarn(m) { appendLog('[WARN] ' + m, 'log-warn'); }
function logErr(m)  { appendLog('[ERR] ' + m, 'log-err'); }
function logInfo(m) { appendLog('[INFO] ' + m, 'log-info'); }

window.clearLog = () => { document.getElementById('log').textContent = ''; };

// ---------------------------------------------------------------------------
// Fetch provisioning protocol from firmware service
// ---------------------------------------------------------------------------

window.fetchProtocol = async function() {
  const base  = document.getElementById('fw-service-url').value.trim().replace(/\/$/, '');
  const token = document.getElementById('api-token').value.trim();
  logInfo(`Fetching ${base}/v1/provisioning-protocol ...`);
  try {
    const r = await fetch(`${base}/v1/provisioning-protocol`, {
      headers: { Authorization: `Bearer ${token}` },
    });
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    const proto = await r.json();
    const tag = proto.firmwareTag;
    if (document.getElementById('fw-tag').value === '' && tag) {
      document.getElementById('fw-tag').value = tag;
    }
    document.getElementById('protocol-display').textContent =
      `firmwareTag: ${proto.firmwareTag}  baud: ${proto.console?.baud}  prompt: "${proto.console?.promptReady}"`;
    logOK(`Protocol fetched — tag=${tag}`);

    // Also fetch build info to get binaryUrl + sha256 (T-02-03).
    await fetchBuildUrl(base, token, tag);
  } catch (e) {
    logErr('fetchProtocol failed: ' + e.message);
  }
};

async function fetchBuildUrl(base, token, tag) {
  logInfo(`GET ${base}/v1/builds/${tag} ...`);
  try {
    const r = await fetch(`${base}/v1/builds/${tag}`, {
      headers: { Authorization: `Bearer ${token}` },
    });
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    const build = await r.json();
    g_binary_url    = build.binaryUrl;
    g_binary_sha256 = build.binarySha256;
    logOK(`Build URL obtained. sha256=${g_binary_sha256?.slice(0,16)}...`);
  } catch (e) {
    logWarn('fetchBuildUrl failed (firmware service may not have this tag cached yet): ' + e.message);
    logWarn('Set binaryUrl manually if needed, or POST /v1/build first.');
  }
}

// ---------------------------------------------------------------------------
// EUI-48 → EUI-64 FFFE insertion (D-05, per IEEE / LoRa Alliance TR007).
// ---------------------------------------------------------------------------

function macToDevEui(macColonHex) {
  // Input:  "3c:dc:75:6f:89:24" (6 bytes, colon-separated)
  // Output: "3CDC75FFFE6F8924" (8 bytes EUI-64, 16 uppercase hex chars)
  const bytes = macColonHex.replace(/[^0-9a-fA-F]/g, '').match(/.{2}/g);
  if (!bytes || bytes.length !== 6) throw new Error('Invalid MAC: ' + macColonHex);
  return [...bytes.slice(0, 3), 'FF', 'FE', ...bytes.slice(3)].join('').toUpperCase();
}

// ---------------------------------------------------------------------------
// Phase 1: Connect → read MAC → (user confirms MAC) → flash
// ---------------------------------------------------------------------------

window.runPhase1 = async function() {
  if (!navigator.serial) {
    logErr('Web Serial API not available. Use Chrome ≥ 89 over HTTPS or localhost.');
    return;
  }

  try {
    // User gesture — port picker.
    g_port = await navigator.serial.requestPort({ filters: [] });
    const info = g_port.getInfo();
    logInfo(`Port selected: VID=0x${info.usbVendorId?.toString(16)} PID=0x${info.usbProductId?.toString(16)}`);

    // Warn if PID doesn't look like native USB-Serial-JTAG.
    // RAK3112 (ESP32-S3 USB-Serial-JTAG) PID = 0x1001.
    if (info.usbProductId && info.usbProductId !== 0x1001) {
      logWarn(`PID 0x${info.usbProductId.toString(16)} is not 0x1001 (native USB-Serial-JTAG). Proceed with caution.`);
    }

    // Create Transport + ESPLoader.
    g_transport = new Transport(g_port, true);
    const terminal = {
      writeLine: (s) => appendLog(s, 'log-info'),
      write:     (s) => appendLog(s, 'log-info'),
      clean:     ()  => {},
    };
    g_esploader = new ESPLoader({
      transport:    g_transport,
      baudrate:     115200,
      terminal,
      debugLogging: false,
    });

    logInfo('Connecting to ROM bootloader ...');
    const chipName = await g_esploader.main();
    logOK(`Chip detected: ${chipName}`);

    // [A2-PROBE] Read EUI-48 MAC.
    // ASSUMPTION A2: esploader.chip.readMac(esploader) is the correct call.
    // Alternatives: esploader.readMac(), esploader.chip.chipName, etc.
    // The exact API must be confirmed on real hardware — record in RUNBOOK A2.
    let mac = null;
    try {
      // Attempt 1: chip.readMac (most likely in esptool-js 0.6.0).
      if (g_esploader.chip && typeof g_esploader.chip.readMac === 'function') {
        mac = await g_esploader.chip.readMac(g_esploader);
        logInfo(`[A2-PROBE] chip.readMac() returned: ${JSON.stringify(mac)}`);
      }
    } catch (e) {
      logWarn('[A2-PROBE] chip.readMac() threw: ' + e.message);
    }
    if (!mac) {
      try {
        // Attempt 2: top-level readMac.
        if (typeof g_esploader.readMac === 'function') {
          mac = await g_esploader.readMac();
          logInfo(`[A2-PROBE] esploader.readMac() returned: ${JSON.stringify(mac)}`);
        }
      } catch (e) {
        logWarn('[A2-PROBE] esploader.readMac() threw: ' + e.message);
      }
    }
    if (!mac) {
      logWarn('[A2-PROBE] Could not read MAC via known APIs — record failure in RUNBOOK A2.');
      mac = 'UNKNOWN';
    }

    // Normalise MAC to colon-hex "xx:xx:xx:xx:xx:xx" format.
    let macColonHex = mac;
    if (Array.isArray(mac)) {
      macColonHex = mac.map(b => b.toString(16).padStart(2, '0')).join(':');
    }
    g_mac = macColonHex;
    logOK(`MAC (EUI-48): ${macColonHex}`);

    // Derive DevEUI.
    try {
      g_deveui = macToDevEui(macColonHex);
      document.getElementById('dev-eui').value = g_deveui;
      logOK(`DevEUI (EUI-64 FFFE): ${g_deveui}`);
    } catch (e) {
      logWarn('DevEUI derivation skipped (MAC format unknown): ' + e.message);
    }

    // Show MAC confirmation gate (T-02-04: confirm board MAC before any write).
    document.getElementById('mac-display').textContent = macColonHex;
    document.getElementById('mac-confirm').style.display = 'block';
    document.getElementById('btn-connect').disabled = true;

  } catch (e) {
    logErr('Phase 1 failed: ' + e.message);
    if (g_transport) { try { await g_transport.disconnect(); } catch (_) {} }
  }
};

window.abortFlash = function() {
  logWarn('Flash aborted by user.');
  document.getElementById('mac-confirm').style.display = 'none';
  document.getElementById('btn-connect').disabled = false;
  if (g_transport) { g_transport.disconnect().catch(() => {}); }
  g_transport = null;
  g_esploader = null;
};

window.confirmAndFlash = async function() {
  document.getElementById('mac-confirm').style.display = 'none';

  const base  = document.getElementById('fw-service-url').value.trim().replace(/\/$/, '');
  const token = document.getElementById('api-token').value.trim();
  const tag   = document.getElementById('fw-tag').value.trim() || 'phase-7-provisioning-green';

  // Fetch binaryUrl if not already available.
  if (!g_binary_url) {
    logInfo('No binaryUrl cached — fetching ...');
    await fetchBuildUrl(base, token, tag);
  }
  if (!g_binary_url) {
    logErr('binaryUrl unavailable. POST /v1/build first or check firmware service.');
    return;
  }

  try {
    // Fetch firmware binary.
    logInfo(`Fetching binary from ${g_binary_url.slice(0, 60)}...`);
    const resp = await fetch(g_binary_url);
    if (!resp.ok) throw new Error(`binary fetch HTTP ${resp.status}`);
    const buf = await resp.arrayBuffer();
    const firmwareBin = new Uint8Array(buf);
    logOK(`Binary fetched: ${firmwareBin.length} bytes`);

    // T-02-03: verify SHA-256 if available.
    if (g_binary_sha256 && typeof crypto.subtle !== 'undefined') {
      const hashBuf = await crypto.subtle.digest('SHA-256', buf);
      const hashHex = Array.from(new Uint8Array(hashBuf)).map(b => b.toString(16).padStart(2, '0')).join('');
      if (hashHex === g_binary_sha256) {
        logOK(`SHA-256 verified: ${hashHex.slice(0, 16)}...`);
      } else {
        logErr(`SHA-256 MISMATCH! Expected ${g_binary_sha256}, got ${hashHex}`);
        throw new Error('Binary integrity check failed (T-02-03)');
      }
    } else {
      logWarn('SHA-256 not available — skipping integrity check');
    }

    // Flash — A1 RESOLVED: app binary offset = 0x10000 (from flasher_args.json).
    logInfo('Writing flash at 0x10000 (eraseAll:false — T-02-01: NEVER erase)...');
    const flashOptions = {
      fileArray: [{ data: firmwareBin, address: 0x10000 }],
      flashMode: 'dio',
      flashFreq: 'keep',
      flashSize: 'keep',
      eraseAll:  false,   // NEVER true — T-02-01 / guardrail §3 #1
      compress:  true,
      reportProgress: (fileIndex, written, total) => {
        const pct = Math.round((written / total) * 100);
        document.getElementById('flash-progress').value = pct;
        if (written === total) logOK(`Flash complete (${total} bytes)`);
      },
    };
    await g_esploader.writeFlash(flashOptions);

    // Hard reset — exit bootloader, boot the application.
    logInfo('Hard reset ...');
    await g_esploader.after('hard_reset');
    await g_transport.disconnect();
    g_transport  = null;
    g_esploader  = null;

    logOK('Phase 1 complete. Port released. Waiting 3s for re-enumeration ...');
    document.getElementById('btn-nvs').disabled = false;

  } catch (e) {
    logErr('Flash failed: ' + e.message);
    if (g_transport) { try { await g_transport.disconnect(); } catch (_) {} }
    g_transport  = null;
    g_esploader  = null;
  }
};

// ---------------------------------------------------------------------------
// Phase 2: NVS console — re-open port, send prov commands, boot-verify.
// ---------------------------------------------------------------------------

window.runPhase2 = async function() {
  // T-02-02: appKey is read once here into a scoped variable and NEVER passed
  // to appendLog(), logInfo(), or any string that reaches the DOM or console.
  const appKey  = document.getElementById('app-key').value.trim();
  const devEui  = document.getElementById('dev-eui').value.trim().toUpperCase();
  const joinEui = '0000000000000000'; // fixed all-zero for ChirpStack (RESEARCH § JoinEUI)
  const mbBaud  = document.getElementById('mb-baud').value.trim()  || '9600';
  const mbPar   = document.getElementById('mb-parity').value.trim() || 'N';
  const mbStop  = document.getElementById('mb-stop').value.trim()   || '1';
  const mbSlave = document.getElementById('mb-slave').value.trim()  || '1';

  if (!appKey || appKey.length !== 32) {
    logErr('appKey must be 32 hex chars'); return;
  }
  if (!devEui || devEui.length !== 16) {
    logErr('devEui must be 16 hex chars (run Phase 1 first)'); return;
  }

  document.getElementById('btn-nvs').disabled = true;

  try {
    // [A5-PROBE] Re-open the retained SerialPort after hard_reset.
    // ASSUMPTION A5: port.open() succeeds on the original SerialPort object.
    // Alternative: navigator.serial.getPorts() + re-select.
    // Record which path works in RUNBOOK A5.
    logInfo('[A5-PROBE] Re-opening port at 115200 baud ...');

    // Wait for re-enumeration.
    await new Promise(r => setTimeout(r, 3000));

    let portOpened = false;
    try {
      await g_port.open({ baudRate: 115200 });
      logOK('[A5-PROBE] Original SerialPort re-opened successfully.');
      portOpened = true;
    } catch (e) {
      logWarn('[A5-PROBE] port.open() on original port failed: ' + e.message);
      logInfo('[A5-PROBE] Trying navigator.serial.getPorts() re-select ...');
      const ports = await navigator.serial.getPorts();
      if (ports.length > 0) {
        g_port = ports[ports.length - 1]; // most-recently-added port
        await g_port.open({ baudRate: 115200 });
        logOK('[A5-PROBE] Re-selected port opened via getPorts().');
        portOpened = true;
      }
    }
    if (!portOpened) throw new Error('Could not re-open serial port after flash');

    const encoder = new TextEncoder();
    const decoder = new TextDecoder();
    const writer  = g_port.writable.getWriter();
    const reader  = g_port.readable.getReader();

    /**
     * Read from port until target string found (or timeout).
     * Returns the accumulated buffer containing target.
     */
    async function waitForLine(target, timeoutMs = 10000) {
      let buffer = '';
      const deadline = Date.now() + timeoutMs;
      while (Date.now() < deadline) {
        const race = await Promise.race([
          reader.read(),
          new Promise(r => setTimeout(() => r({ value: undefined, done: false, timedOut: true }), 150)),
        ]);
        if (race.timedOut) continue;
        if (race.done) break;
        if (race.value) {
          const chunk = decoder.decode(race.value);
          buffer += chunk;
          // Print received data (excluding appKey — T-02-02).
          if (!chunk.includes(appKey)) {
            appendLog(chunk.replace(/\r/g, ''), 'log-info');
          }
          if (buffer.includes(target)) return buffer;
        }
      }
      throw new Error(`Timeout waiting for "${target}" (got: ${buffer.slice(-100)})`);
    }

    /**
     * Send a command line.
     * [A3-PROBE] Line terminator: try \r\n first (standard VT100).
     * Record which terminator works in RUNBOOK A3.
     * T-02-02: appKey is never passed as a string to logInfo/appendLog.
     */
    async function sendCmd(cmd, logSafe = true) {
      // [A3-PROBE] Using \r\n — change to \n if commands are ignored on hardware.
      const line = cmd + '\r\n';
      await writer.write(encoder.encode(line));
      if (logSafe) logInfo(`> ${cmd}`);
      // If logSafe is false, the command contains the appKey — do NOT log it.
    }

    // Wait for firmware boot + console prompt.
    logInfo('Waiting for "esp> " prompt (up to 15s) ...');
    await waitForLine('esp> ', 15000);
    logOK('Console ready (esp> )');

    // Provision Modbus config.
    await sendCmd(`prov modbus ${mbBaud} ${mbPar} ${mbStop} ${mbSlave}`);
    await waitForLine('esp> ', 5000);

    // Provision credentials.
    // T-02-02: do NOT log the appKey — logSafe=false suppresses the log line.
    const credsCmd = `prov creds ${devEui} ${joinEui} ${appKey}`;
    logInfo(`> prov creds ${devEui} ${joinEui} <appKey:redacted>`);
    await sendCmd(credsCmd, /* logSafe */ false);
    await waitForLine('esp> ', 5000);

    // Verify (prov show — appKey will be redacted by firmware).
    await sendCmd('prov show');
    await waitForLine('esp> ', 5000);

    // [A4-PROBE] Reboot for boot-verify.
    // ASSUMPTION A4: "restart" or "esp_restart" is available as a console command.
    // Alternative: hard reset via RTS pin toggle.
    // Record which works in RUNBOOK A4.
    logInfo('[A4-PROBE] Sending reboot command ...');
    try {
      await sendCmd('restart');
    } catch (_) {
      logWarn('[A4-PROBE] "restart" failed — trying "esp_restart" ...');
      await sendCmd('esp_restart');
    }

    // Boot-verify: read log for required markers.
    logInfo('Reading boot log for markers: [creds:NVS] PSRAM modbus (up to 20s) ...');
    const bootLog = await waitForLine('[creds:NVS]', 20000);

    const markers = ['[creds:NVS]', 'PSRAM', 'modbus'];
    const results = markers.map(m => ({ marker: m, found: bootLog.includes(m) }));
    const allOK   = results.every(r => r.found);

    const resultDiv = document.getElementById('boot-verify-result');
    results.forEach(r => {
      const line = `${r.found ? '✓' : '✗'} ${r.marker}`;
      resultDiv.innerHTML += `<div style="color:${r.found ? '#66ff66' : '#ff4444'}">${line}</div>`;
      if (r.found) logOK(`Boot marker: ${r.marker}`);
      else         logErr(`Boot marker MISSING: ${r.marker}`);
    });

    if (allOK) {
      logOK('ALL boot-verify markers present. NVS provisioning confirmed.');
    } else {
      logErr('Some boot markers missing. Check firmware build or prov commands.');
    }

    // Release port.
    reader.releaseLock();
    writer.releaseLock();
    await g_port.close();
    logInfo('Port closed. Phase 2 complete.');

  } catch (e) {
    logErr('Phase 2 failed: ' + e.message);
    try { await g_port.close(); } catch (_) {}
  } finally {
    document.getElementById('btn-nvs').disabled = false;
  }
};
