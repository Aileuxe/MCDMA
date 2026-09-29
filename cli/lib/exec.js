'use strict';
// Process helpers: local shell, ssh (multiplexed), privileged runs through the
// macOS administrator prompt, and line-oriented long-lived endpoints.
const { spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const readline = require('readline');

const SSH = '/usr/bin/ssh';
let cmDir = null;
let sshConfig = null;
function setSshConfig(file) {
  if (cmDir) throw new Error('Cannot change SSH configuration while connections are active');
  if (file == null) { sshConfig = null; return; }
  const resolved = path.resolve(file);
  if (!fs.statSync(resolved).isFile()) throw new Error('SSH configuration must be a readable file');
  fs.accessSync(resolved, fs.constants.R_OK);
  sshConfig = resolved;
}

function sshBase() {
  if (!cmDir) {
    // macOS has a 104-byte Unix socket path limit, including ssh's suffix.
    cmDir = fs.mkdtempSync('/tmp/mcdma-ssh-');
    fs.chmodSync(cmDir, 0o700);
  }
  return [
    ...(sshConfig ? ['-F', sshConfig] : []),
    '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5', '-o', 'ServerAliveInterval=15',
    '-o', 'StrictHostKeyChecking=accept-new',
    '-o', 'ControlMaster=auto', '-o', `ControlPath=${cmDir}/%C`, '-o', 'ControlPersist=600'
  ];
}

function run(cmd, args = [], opts = {}) {
  const { timeoutMs = 10000, input = null, env = process.env, cwd } = opts;
  return new Promise((resolve) => {
    let out = '', err = '', settled = false, p;
    const done = (v) => { if (!settled) { settled = true; clearTimeout(timer); resolve(v); } };
    try { p = spawn(cmd, args, { stdio: [input == null ? 'ignore' : 'pipe', 'pipe', 'pipe'], env, cwd }); }
    catch (e) { return resolve({ code: -1, out: '', err: String(e), timedOut: false }); }
    const timer = setTimeout(() => {
      try { p.kill('SIGKILL'); } catch {}
      done({ code: -1, out, err: err + '\n[timed out]', timedOut: true });
    }, timeoutMs);
    p.stdout.on('data', (d) => { out += d; });
    p.stderr.on('data', (d) => { err += d; });
    p.on('error', (e) => done({ code: -1, out, err: err + String(e), timedOut: false }));
    p.on('close', (code) => done({ code, out, err, timedOut: false }));
    if (input != null) p.stdin.end(input);
  });
}

const sh = (script, opts) => run('/bin/bash', ['-c', script], opts);

function validateSshHost(host) {
  if (typeof host !== 'string' || !host || /^-/.test(host) || /[\s\x00]/.test(host)) throw new Error('Invalid SSH host or alias');
  return host;
}

async function ssh(host, script, opts = {}) {
  validateSshHost(host);
  const r = await run(SSH, [...sshBase(), host, script], opts);
  if (r.code === 255 && !r.timedOut) { // connection-level failure: retry once without the mux
    return run(SSH, ['-o', 'ControlPath=none', '-o', 'ControlMaster=no', ...sshBase(), host, script], opts);
  }
  return r;
}

function sshEnd(host) { validateSshHost(host); return run(SSH, [...sshBase(), '-O', 'exit', host], { timeoutMs: 2000 }); }

// Privileged script. The command-line tool installs a sudo-based runner; the
// fallback is the macOS administrator dialog (osascript) for callers that embed
// the engine in an application.
let privilegeRunner = null; // async (file, prompt) => { code, out, err, cancelled? }
function setPrivilegeRunner(fn) { privilegeRunner = fn; }

// Like run(), but stdin is inherited so a terminal prompt (sudo) can be answered.
function runInherit(cmd, args = [], opts = {}) {
  const { timeoutMs = 15 * 60 * 1000, env = process.env } = opts;
  return new Promise((resolve) => {
    let out = '', err = '', settled = false, p;
    const done = (v) => { if (!settled) { settled = true; clearTimeout(timer); resolve(v); } };
    try { p = spawn(cmd, args, { stdio: ['inherit', 'pipe', 'pipe'], env }); }
    catch (e) { return resolve({ code: -1, out: '', err: String(e), timedOut: false }); }
    const timer = setTimeout(() => { try { p.kill('SIGKILL'); } catch {} done({ code: -1, out, err: err + '\n[timed out]', timedOut: true }); }, timeoutMs);
    p.stdout.on('data', (d) => { out += d; });
    p.stderr.on('data', (d) => { err += d; process.stderr.write(d); });
    p.on('error', (e) => done({ code: -1, out, err: err + String(e), timedOut: false }));
    p.on('close', (code) => done({ code, out, err, timedOut: false }));
  });
}

async function sudoRunner(file, prompt) {
  if (typeof process.getuid === 'function' && process.getuid() === 0) return run('/bin/bash', [file], { timeoutMs: 15 * 60 * 1000 });
  const interactive = process.stdin.isTTY && process.stderr.isTTY;
  const args = interactive ? ['-p', `${prompt}\nPassword: `, '/bin/bash', file] : ['-n', '/bin/bash', file];
  const r = await runInherit('/usr/bin/sudo', args);
  if (r.code !== 0 && /password is required|a terminal is required|no tty present/i.test(r.err)) return { code: -2, out: r.out, err: 'administrator rights needed: run again with sudo or from a terminal', cancelled: true };
  return r;
}

async function adminRun(script, prompt = 'mcdma needs administrator access.') {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'mcdma-admin-'));
  fs.chmodSync(directory, 0o700);
  const file = path.join(directory, 'run.sh');
  try {
    fs.writeFileSync(file, '#!/bin/bash\nset -u\n' + script + '\n', { mode: 0o700, flag: 'wx' });
    if (privilegeRunner) return await privilegeRunner(file, prompt);
    const aq = (s) => '"' + String(s).replace(/\\/g, '\\\\').replace(/"/g, '\\"') + '"';
    const osa = `do shell script ${aq('/bin/bash ' + "'" + file.replace(/'/g, "'\\''") + "' 2>&1")} with administrator privileges with prompt ${aq(prompt)}`;
    const r = await run('/usr/bin/osascript', ['-e', osa], { timeoutMs: 15 * 60 * 1000 });
    if (/User canceled|-128/.test(r.err)) return { code: -2, out: r.out, err: 'cancelled', cancelled: true };
    return r;
  } finally { fs.rmSync(directory, { recursive: true, force: true }); }
}

// A host is either this Mac or a machine reached over ssh; both expose sh().
class Host {
  constructor(kind, alias) { this.kind = kind; this.alias = kind === 'ssh' ? validateSshHost(alias) : null; }
  get label() { return this.kind === 'local' ? 'this Mac' : this.alias; }
  sh(script, opts) { return this.kind === 'local' ? sh(script, opts) : ssh(this.alias, script, opts); }
  spawnArgs(command) { // [cmd, args] for a long-lived process
    return this.kind === 'local'
      ? ['/bin/bash', ['-c', command]]
      : [SSH, [...sshBase(), this.alias, command]];
  }
}

// Long-lived process speaking newline-delimited text on stdin/stdout.
class Endpoint {
  constructor(host, command, log) {
    this.host = host; this.command = command; this.log = log || (() => {});
    this.lines = []; this.waiters = []; this.stderr = ''; this.closed = false; this.exitCode = null;
    const [cmd, args] = host.spawnArgs(command);
    this.p = spawn(cmd, args, { stdio: ['pipe', 'pipe', 'pipe'] });
    this.rl = readline.createInterface({ input: this.p.stdout });
    this.rl.on('line', (l) => { this.log(`${host.label} < ${l}`); const w = this.waiters.shift(); w ? w(l) : this.lines.push(l); });
    this.p.stderr.on('data', (d) => { this.stderr += d; });
    this.p.on('close', (code) => { this.closed = true; this.exitCode = code; while (this.waiters.length) this.waiters.shift()(null); });
    this.p.on('error', () => { this.closed = true; });
  }
  line(timeoutMs = 30000) {
    if (this.lines.length) return Promise.resolve(this.lines.shift());
    if (this.closed) return Promise.resolve(null);
    return new Promise((resolve) => {
      const t = setTimeout(() => { const i = this.waiters.indexOf(fn); if (i >= 0) this.waiters.splice(i, 1); resolve(null); }, timeoutMs);
      const fn = (l) => { clearTimeout(t); resolve(l); };
      this.waiters.push(fn);
    });
  }
  send(text) { this.log(`${this.host.label} > ${text}`); try { this.p.stdin.write(text + '\n'); } catch {} }
  stop(timeoutMs = 8000) {
    return new Promise((resolve) => {
      if (this.closed) return resolve(this.exitCode);
      try { this.p.stdin.end(); } catch {}
      const t = setTimeout(() => { try { this.p.kill('SIGKILL'); } catch {} resolve(-1); }, timeoutMs);
      this.p.once('close', (code) => { clearTimeout(t); resolve(code); });
    });
  }
}

function dispose(hosts) {
  for (const h of hosts) if (h.kind === 'ssh') sshEnd(h.alias);
  try { if (cmDir) fs.rmSync(cmDir, { recursive: true, force: true }); } catch {}
  cmDir = null;
}

module.exports = { run, sh, ssh, sshEnd, adminRun, setPrivilegeRunner, setSshConfig, sudoRunner, runInherit, Host, Endpoint, dispose };
