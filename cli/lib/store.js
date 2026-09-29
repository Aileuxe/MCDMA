'use strict';
// Persisted settings: ~/Library/Application Support/MCDMA/settings.json
const fs = require('fs');
const path = require('path');
const os = require('os');

const DEFAULTS = {
  studio: { mode: 'local', host: '' },                // 'ssh' manages a remote Mac read-only
  sparks: [],                                          // [{ id, host }]
  linuxLinks: [],                                      // [{id, a:{node,iface}, b:{node,iface}}]
  lastLinuxTests: {},
  macs: [],                                            // additional Macs with cards, managed over ssh: [{ id, host }]
  enableInProgress: false,                             // the one-click flow was started and may resume after a restart
  mapping: {},                                         // studio iface -> { spark, iface } | null
  wiringVerified: {},                                  // studio iface -> { spark, iface, at }
  lastTests: {},                                       // studio iface -> result
  tools: { macPeer: '', sparkPeer: '', linuxPeerPaths: {}, keepalive: '', provider: '', macBw: '', sparkBw: '', macChecker: '' },
  test: { payload: 4096, mtu: 1024, arm: 'bf64', iterations: 1000 },
  monitor: { pollMs: 1000, heavyEveryTicks: 5, extraLinks: [] },
  driverPackage: '',
  demo: false,
  firstRunDone: false
};

class Store {
  constructor(dir) {
    this.dir = dir; this.file = path.join(dir, 'settings.json');
    this.data = JSON.parse(JSON.stringify(DEFAULTS));
    try {
      const saved = JSON.parse(fs.readFileSync(this.file, 'utf8'));
      this.data = deepMerge(this.data, saved);
    } catch {}
  }
  get() { return this.overlay ? deepMerge(this.data, this.overlay) : this.data; }
  set(patch) { this.data = deepMerge(this.data, patch); this.save(); return this.get(); }
  override(patch) { this.overlay = deepMerge(this.overlay || {}, patch); } // in-memory only, never saved
  replace(key, value) { this.data[key] = value; this.save(); return this.data; }
  save() {
    try { fs.mkdirSync(this.dir, { recursive: true }); fs.writeFileSync(this.file, JSON.stringify(this.data, null, 2)); } catch {}
  }
}

function deepMerge(a, b) {
  if (Array.isArray(b) || b === null || typeof b !== 'object') return b === undefined ? a : b;
  const out = { ...(a && typeof a === 'object' && !Array.isArray(a) ? a : {}) };
  for (const [k, v] of Object.entries(b)) out[k] = deepMerge(out[k], v);
  return out;
}

// ssh host aliases from ~/.ssh/config, for the "add Spark" picker.
function sshConfigHosts() {
  const hosts = [];
  try {
    const text = fs.readFileSync(path.join(os.homedir(), '.ssh', 'config'), 'utf8');
    let cur = null;
    for (const raw of text.split('\n')) {
      const line = raw.trim();
      const m = line.match(/^Host\s+(.+)$/i);
      if (m) { for (const h of m[1].split(/\s+/)) if (!/[*?]/.test(h)) { cur = { host: h, hostname: null, user: null }; hosts.push(cur); } else cur = null; continue; }
      if (!cur) continue;
      const kv = line.match(/^(\w+)\s+(.+)$/);
      if (!kv) continue;
      if (/^hostname$/i.test(kv[1])) cur.hostname = kv[2];
      if (/^user$/i.test(kv[1])) cur.user = kv[2];
    }
  } catch {}
  return hosts;
}

module.exports = { Store, DEFAULTS, sshConfigHosts, deepMerge };
