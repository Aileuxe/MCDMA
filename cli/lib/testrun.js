'use strict';
// Real RDMA transfer test between a Mac port and a Spark port, driving the
// project's two peer programs (native-verbs-peer on the Mac, verbs-peer on the
// Spark) through their stdin/stdout rendezvous, plus the GPU keep-alive runner.
const { Endpoint, Host } = require('./exec');
const { TOOLS_DIR, PROVIDER_PATH } = require('./macinfo');
const { normIp6 } = require('./parse');
const { validateMode, validateTestSettings, readTrace } = require('./verification');

const ARMS = {
  bf64: { MCDMA_CQ_MAP: '2', MCDMA_USER_POST: '1', MCDMA_USER_BF: '64' },   // userspace BlueFlame fast path
  direct: { MCDMA_CQ_MAP: '2', MCDMA_USER_POST: '1', MCDMA_USER_BF: '0' },
  kernel: { MCDMA_CQ_MAP: '0', MCDMA_USER_POST: '0', MCDMA_USER_BF: '0' }                                                               // kernel submission path
};
const q = (s) => "'" + String(s).replace(/'/g, "'\\''") + "'";

async function firstExecutable(host, paths) {
  for (const p of paths.filter(Boolean)) {
    const r = await host.sh(`test -x ${q(p)} && echo yes`, { timeoutMs: 8000 });
    if (/yes/.test(r.out)) return p;
  }
  return null;
}

function parseLatency(line) {
  const m = (line || '').match(/^LATENCY op=(write|read) bytes=(\d+) samples=(\d+) warmup=(\d+) qd=(\d+) min_us=([\d.]+) median_us=([\d.]+) p95_us=([\d.]+) p99_us=([\d.]+) max_us=([\d.]+) mean_us=([\d.]+)/);
  if (!m) return null;
  return { op: m[1], bytes: Number(m[2]), samples: Number(m[3]), min: Number(m[6]), median: Number(m[7]), p95: Number(m[8]), p99: Number(m[9]), max: Number(m[10]), mean: Number(m[11]) };
}

function descriptor(line) {
  const f = (line || '').trim().split(/\s+/);
  if (f[0] !== 'ENDPOINT' || f.length < 7) return null;
  return { qpn: f[1], psn: f[2], rkey: f[3], addr: f[4], len: f[5], gid: normIp6(f[6]) };
}

async function runTransferTest({ studioHost, link, settings, onProgress = () => {}, latency = true }) {
  const log = [];
  const note = (m) => { log.push(m); onProgress(m); };
  const errors = [];
  const endpoints = [];
  const result = { at: Date.now(), spark: link.spark.spark, iface: link.spark.iface, studioIface: link.studio.iface, mac: link.mac ? link.mac.id : 'local', passed: false, errors, latency: null, log,
    payload: settings.test.payload, mtu: settings.test.mtu, arm: settings.test.arm };
  const sparkHost = new Host('ssh', link.sparkHost);
  if (!studioHost) studioHost = link.mac && link.mac.kind === 'ssh' ? new Host('ssh', link.mac.host) : new Host('local');
  try {
    validateTestSettings(settings.test, latency);
    const macClient = await firstExecutable(studioHost, [settings.tools.macPeer, `${TOOLS_DIR}/native-verbs-peer`]);
    if (!macClient) throw new Error('native-verbs-peer not found on the Mac (install the driver package or set the path in Settings)');
    const sparkClient = await firstExecutable(sparkHost, [settings.tools.sparkPeer, `${TOOLS_DIR}/verbs-peer`, ...(link.spark.peerTools || [])]);
    if (!sparkClient) throw new Error(`verbs-peer not found on ${link.sparkName} (use "Install test tool on Spark")`);
    if (!link.studio.rdmaDevice) throw new Error('the Mac port has no RDMA device');
    if (link.spark.gidIndex == null) throw new Error('the Spark port has no RoCE v2 GID');
    const provider = settings.tools.provider || PROVIDER_PATH;
    const arm = ARMS[settings.test.arm] ? settings.test.arm : 'bf64';
    const env = { MCDMA_PAYLOAD_BYTES: String(settings.test.payload), MCDMA_PATH_MTU: String(settings.test.mtu), MCDMA_LATENCY_PROFILE: '0' };
    const macEnv = { IBV_DRIVERS: provider.replace(/-rdmav34\.so$/, ''), ...env, ...ARMS[arm] };
    const envText = (o) => Object.entries(o).map(([k, v]) => `${k}=${q(v)}`).join(' ');
    note(`Starting ${macClient.split('/').pop()} on the Mac (${link.studio.rdmaDevice}, ${arm} path)`);
    const mac = new Endpoint(studioHost, `env ${envText(macEnv)} ${q(macClient)} ${q(link.studio.rdmaDevice)} 0 initiator`, (l) => log.push(l));
    endpoints.push(mac);
    note(`Starting verbs-peer on ${link.sparkName} (${link.spark.rdmaDevice}, GID index ${link.spark.gidIndex})`);
    const spark = new Endpoint(sparkHost, `env ${envText({ ...env, MCDMA_RDMA_PORT: String(link.spark.rdmaPort || 1) })} ${q(sparkClient)} ${q(link.spark.rdmaDevice)} ${link.spark.gidIndex}`, (l) => log.push(l));
    endpoints.push(spark);
    const local = descriptor(await mac.line(25000));
    if (!local) throw new Error(`Mac endpoint did not start: ${(mac.stderr || '').trim().split('\n').pop() || 'no ENDPOINT line'}`);
    const remote = descriptor(await spark.line(25000));
    if (!remote) throw new Error(`Spark endpoint did not start: ${(spark.stderr || '').trim().split('\n').pop() || 'no ENDPOINT line'}`);
    if (local.gid !== normIp6(link.expected.studioLinkLocal)) throw new Error(`Mac GID ${local.gid} is not the expected ${link.expected.studioLinkLocal}`);
    if (remote.gid !== normIp6(link.spark.gid)) throw new Error(`Spark GID ${remote.gid} is not the expected ${link.spark.gid}`);
    note(`Mac QP ${local.qpn} ↔ Spark QP ${remote.qpn}; connecting`);
    mac.send(`${remote.qpn} ${remote.psn} ${remote.gid}`);
    spark.send(`${local.qpn} ${local.psn} ${local.gid}`);
    if ((await mac.line(20000)) !== 'READY') throw new Error('Mac QP did not reach RTS');
    if ((await spark.line(20000)) !== 'READY') throw new Error('Spark QP did not reach RTS');
    const p = settings.test.payload;
    note(`Mac-initiated ${p}-byte WRITE and READ${latency ? ' with latency sampling' : ''}`);
    mac.send(`${latency ? 'INITIATEBENCH' : 'INITIATE'} ${remote.rkey} ${remote.addr} ${remote.len}`);
    const fwd = await mac.line(60000);
    if (fwd !== `NATIVE_FORWARD write=1 read=1 verified=${p}`) {
      spark.send('VERIFY'); await spark.line(10000);
      throw new Error(`Mac-initiated transfer failed: ${fwd || 'no reply'} ${(mac.stderr || '').trim().split('\n').filter((l) => /CQ|timeout|status/.test(l)).slice(-1)[0] || ''}`.trim());
    }
    const lat = { mac: {}, spark: {} };
    if (latency) {
      for (let i = 0; i < 2; i++) { const s = parseLatency(await mac.line(60000)); if (!s) throw new Error('missing Mac latency summary'); lat.mac[s.op] = s; }
      result.traces = { mac: await readTrace(studioHost, await mac.line(10000), settings.test.payload) };
      note(`Mac: WRITE ${lat.mac.write.median} µs, READ ${lat.mac.read.median} µs (median)`);
    }
    note(`Spark-initiated ${p}-byte WRITE and READ`);
    spark.send(`${latency ? 'ROUNDTRIPBENCH' : 'ROUNDTRIP'} ${local.rkey} ${local.addr} ${local.len}`);
    const rev = await spark.line(60000);
    if (rev !== `PEER_RESULT forward=${p} write=1 read=1 reverse=${p}`) throw new Error(`Spark-initiated transfer or payload verification failed: ${rev || 'no reply'}`);
    if (latency) {
      for (let i = 0; i < 2; i++) { const s = parseLatency(await spark.line(60000)); if (!s) throw new Error('missing Spark latency summary'); lat.spark[s.op] = s; }
      result.traces.spark = await readTrace(sparkHost, await spark.line(10000), settings.test.payload);
      note(`Spark: WRITE ${lat.spark.write.median} µs, READ ${lat.spark.read.median} µs (median)`);
    }
    mac.send('CHECKREVERSE');
    const chk = await mac.line(20000);
    if (chk !== `NATIVE_REVERSE verified=${p}`) throw new Error(`Mac did not observe the Spark payload: ${chk || 'no reply'}`);
    if (latency) for (const side of ['mac', 'spark']) for (const op of ['write', 'read']) {
      const row = lat[side][op];
      if (!row || row.bytes !== p || row.samples !== 1000) throw new Error('Latency summary does not match the requested test');
    }
    result.latency = latency ? lat : null;
    result.passed = true;
    note('Both directions verified byte for byte');
  } catch (e) {
    errors.push(e.message || String(e));
    note(`Failed: ${e.message || e}`);
  } finally {
    for (const ep of endpoints) {
      const code = await ep.stop();
      if (code !== 0 && result.passed) { errors.push(`${ep.host.label}: endpoint exit ${code}`); result.passed = false; }
    }
  }
  if (result.passed) {
    try { validateMode(endpoints[0].stderr || '', settings.test.arm); result.modeConfirmed = true; }
    catch (e) { errors.push(e.message); }
  }
  result.passed = result.passed && !errors.length;
  return result;
}

// Installs the bundled Linux peer tool on a Spark (needs the driver package archive).
async function installSparkPeer({ archive, sparkHostAlias }) {
  const fs = require('fs'); const os = require('os'); const path = require('path'); const { run } = require('./exec');
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'mcdma-peer-'));
  try {
    const x = await run('/usr/bin/tar', ['xzf', archive, '-C', tmp, './tools/linux-arm64/verbs-peer'], { timeoutMs: 20000 });
    const file = path.join(tmp, 'tools/linux-arm64/verbs-peer');
    if (x.code !== 0 || !fs.existsSync(file)) return { ok: false, message: 'the driver package has no Linux verbs-peer tool' };
    const data = fs.readFileSync(file);
    const r = await run('/usr/bin/ssh', ['-o', 'BatchMode=yes', '-o', 'ConnectTimeout=6', sparkHostAlias,
      `mkdir -p ${TOOLS_DIR} && cat > ${TOOLS_DIR}/verbs-peer.tmp && chmod 755 ${TOOLS_DIR}/verbs-peer.tmp && mv ${TOOLS_DIR}/verbs-peer.tmp ${TOOLS_DIR}/verbs-peer && ${TOOLS_DIR}/verbs-peer 2>&1 | head -1; echo installed`], { input: data, timeoutMs: 30000 });
    return r.code === 0 && /installed/.test(r.out) ? { ok: true, message: `verbs-peer installed at ${TOOLS_DIR}/verbs-peer on ${sparkHostAlias}` } : { ok: false, message: (r.err || r.out).trim() || 'copy failed' };
  } finally { fs.rmSync(tmp, { recursive: true, force: true }); }
}

class Keepalive {
  constructor() { this.ep = null; this.startedAt = null; this.path = null; }
  get running() { return !!(this.ep && !this.ep.closed); }
  async start(studioHost, settings) {
    if (this.running) return { ok: true, message: 'already running' };
    const path = await firstExecutable(studioHost, [settings.tools.keepalive, `${TOOLS_DIR}/fabric-keepalive`]);
    if (!path) return { ok: false, message: 'fabric-keepalive not found on the Mac' };
    this.ep = new Endpoint(studioHost, `exec ${q(path)} 0 small`);
    this.startedAt = Date.now(); this.path = path;
    await new Promise((r) => setTimeout(r, 800));
    if (this.ep.closed) return { ok: false, message: `fabric-keepalive exited: ${(this.ep.stderr || '').trim().split('\n').pop() || `code ${this.ep.exitCode}`}` };
    return { ok: true, message: `keep-alive running (${path})` };
  }
  async stop() { if (this.ep) { try { this.ep.p.kill('SIGTERM'); } catch {} await this.ep.stop(3000); } this.ep = null; return { ok: true, message: 'keep-alive stopped' }; }
  status() { return { running: this.running, since: this.startedAt, path: this.path }; }
}

module.exports = { runTransferTest, installSparkPeer, Keepalive, parseLatency, descriptor, ARMS };
