#!/usr/bin/env node
/**
 * dsh-pet-dial bridge
 *
 * Mirrors the DSH desktop pet's work status onto the M5Stack Dial.
 *
 *   node bridge.mjs [--port COM5] [--url http://127.0.0.1:52202] [--pet main]
 *
 * The dsh-pet host plugin exposes a long-poll endpoint:
 *   GET /dsh-pet-7340/state?since=<counter>&timeout=<ms>
 * whose `sections.workStatus.data.state` tracks the running turn. We forward
 * every change to the device as a one-line command over USB CDC:
 *
 *   S idle|thinking|working|result|waiting|success|error
 *
 * The device replies with "READY ..." at boot and "ACK S ..." per command.
 */
import fs from 'node:fs';
import { execFileSync } from 'node:child_process';

const args = process.argv.slice(2);
const opt = (name, def) => {
  const i = args.indexOf(`--${name}`);
  return i >= 0 && args[i + 1] ? args[i + 1] : def;
};

const baseUrl = (opt('url', process.env.DSH_WEB_URL || 'http://127.0.0.1:52202')).replace(/\/$/, '');
const petId = opt('pet', 'main');
const stateUrl = `${baseUrl}/dsh-pet-7340/state`;
let portName = opt('port', '');

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 19), ...a);

// ---------------------------------------------------------------- serial
class Dial {
  constructor(name) {
    this.name = name;
    this.fd = null;
    this.buf = '';
    this.onLine = null;
  }
  open() {
    this.fd = fs.openSync(`\\\\.\\${this.name}`, 'r+');
    this.reader = fs.createReadStream(null, { fd: this.fd, autoClose: false });
    this.reader.on('data', (chunk) => {
      this.buf += chunk.toString('utf8');
      let i;
      while ((i = this.buf.indexOf('\n')) >= 0) {
        const line = this.buf.slice(0, i).trim();
        this.buf = this.buf.slice(i + 1);
        if (line) this.onLine?.(line);
      }
    });
    this.reader.on('error', () => {});
  }
  write(cmd) {
    if (this.fd == null) return false;
    try {
      fs.writeSync(this.fd, cmd + '\n');
      return true;
    } catch {
      return false;
    }
  }
  close() {
    try { this.reader?.destroy(); } catch {}
    try { if (this.fd != null) fs.closeSync(this.fd); } catch {}
    this.fd = null;
  }
}

/** Ask a port for the firmware banner; returns true when it is our pet. */
async function probe(name) {
  const d = new Dial(name);
  let seen = '';
  try {
    d.open();
    d.onLine = (line) => { seen += line + '\n'; };
    d.write('I');
    for (let i = 0; i < 12 && !seen.includes('INFO anims='); i++) await sleep(100);
    return seen.includes('INFO anims=');
  } catch {
    return false;
  } finally {
    d.close();
  }
}

async function listPorts() {
  try {
    const out = execFileSync('powershell', [
      '-NoProfile', '-Command',
      'Get-CimInstance Win32_SerialPort | Where-Object { $_.PNPDeviceID -match "VID_303A&PID_1001" } | ForEach-Object { $_.DeviceID }',
    ], { encoding: 'utf8' });
    return out.split(/\s+/).filter((p) => /^COM\d+$/.test(p));
  } catch {
    return [];
  }
}

async function findDial() {
  if (portName) return portName;
  for (const p of await listPorts()) {
    if (await probe(p)) {
      log('found dsh-pet on', p);
      return p;
    }
  }
  return null;
}

// ---------------------------------------------------------------- main loop
let dial = null;
let lastState = null;
let lastCounter = 0;

async function ensureDial() {
  if (dial?.fd != null) return true;
  const name = await findDial();
  if (!name) return false;
  dial = new Dial(name);
  try {
    dial.open();
  } catch (e) {
    log('open failed:', e.message);
    dial = null;
    return false;
  }
  dial.onLine = (line) => {
    if (line.startsWith('ACK') || line.startsWith('ERR')) log('<-', line);
  };
  dial.write('I');
  return true;
}

async function pollState() {
  // the host caps the long-poll wait at 10000 ms; anything larger is rejected
  // with HTTP 200 + {"ok":false,...}, so keep it just under the limit.
  const url = `${stateUrl}?since=${lastCounter}&timeout=9000`;
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), 15000);
  try {
    const res = await fetch(url, { signal: ctrl.signal });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const data = await res.json();
    if (data?.ok === false) throw new Error(`state rejected: ${data.reason || data.message}`);
    lastCounter = data.counter ?? lastCounter;
    return data;
  } finally {
    clearTimeout(timer);
  }
}

function sendState(state) {
  if (!dial || state === lastState) return;
  if (dial.write(`S ${state}`)) {
    lastState = state;
    log('->', state);
  } else {
    log('write failed for', state);
  }
}

async function main() {
  log('bridge starting; pet =', petId);
  if (!(await ensureDial())) log('dial not found yet, will keep retrying');
  else {
    // reset any stale state on the device
    dial.write('S idle');
    lastState = 'idle';
  }

  for (;;) {
    try {
      if (!(await ensureDial())) {
        await sleep(2000);
        continue;
      }
      const data = await pollState();
      const ws = data?.sections?.workStatus?.data;
      const state = ws?.state || 'idle';
      sendState(state);
    } catch (e) {
      if (e.name !== 'AbortError') log('poll error:', e.message);
      // connection problems: drop the serial handle and re-probe
      dial?.close();
      dial = null;
      lastState = null;
      await sleep(1500);
    }
  }
}

process.on('SIGINT', () => {
  dial?.close();
  process.exit(0);
});

main();
