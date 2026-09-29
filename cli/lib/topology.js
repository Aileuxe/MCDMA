'use strict';
// Topology model across any number of Macs (each with a ConnectX card) and
// Sparks: which Mac port is cabled to which Spark port, what each side must be
// configured with, and how far along each link is. Pure functions.
const { eui64 } = require('./parse');
const { driverIdentity } = require('./verification');
const { buildLinuxLinks } = require('./linuxlinks');

const portKey = (macId, iface) => `${macId}:${iface}`;

function macPorts(macs) {
  const out = [];
  for (const m of macs) {
    const info = m.info;
    if (!info || !info.ok) continue;
    for (const p of info.ports || []) {
      out.push({ ...p, macId: m.id, macKind: m.kind, macHost: m.host || null, macLabel: m.label || (info.chip && info.chip.hostname) || m.id,
        key: p.iface ? portKey(m.id, p.iface) : portKey(m.id, `pci-${p.pci}`) });
    }
  }
  return out;
}

function candidatePorts(sparks) {
  // Spark ports (primary PCI view only) that could face a Mac: not on a cable
  // that another Spark also reads (those are Spark-to-Spark links).
  const sn = {};
  for (const sp of sparks) for (const p of sp.ports || []) if (p.primary && p.cable && p.cable.sn) (sn[p.cable.sn] = sn[p.cable.sn] || new Set()).add(sp.id);
  const out = [];
  for (const sp of sparks) {
    if (!sp.reachable) continue;
    for (const p of sp.ports || []) {
      if (!p.primary) continue;
      if (p.cable && p.cable.sn && sn[p.cable.sn].size > 1) continue;
      out.push({ spark: sp.id, host: sp.host, hostname: sp.hostname, iface: p.iface, mac: p.mac, gid: p.gid, gidIndex: p.gidIndex,
        rdmaDevice: p.rdmaDevice, rdmaPort: p.rdmaPort || 1, speedGbps: p.speedGbps, link: p.link, cable: p.cable, eui64: p.eui64, views: p.views,
        neighbours: p.neighbours, peerTools: sp.peerTools || [], peerToolHashes: sp.peerToolHashes || {}, bootId: sp.bootId || null, kind: sp.kind || 'spark', key: `${sp.id}/${p.iface}` });
    }
  }
  return out;
}

function sparkLinks(sparks) {
  const bySn = {};
  for (const sp of sparks) for (const p of sp.ports || []) {
    if (!p.primary || !p.cable || !p.cable.sn) continue;
    (bySn[p.cable.sn] = bySn[p.cable.sn] || []).push({ spark: sp.id, hostname: sp.hostname, iface: p.iface, mac: p.mac, speedGbps: p.speedGbps, link: p.link, rdmaDevice: p.rdmaDevice, pn: p.cable.pn });
  }
  const links = [];
  for (const [sn, ends] of Object.entries(bySn)) {
    const distinct = [...new Set(ends.map((e) => e.spark))];
    if (distinct.length < 2) continue;
    const a = ends.find((e) => e.spark === distinct[0]), b = ends.find((e) => e.spark === distinct[1]);
    links.push({ id: `${a.spark}-${b.spark}-${sn}`, a, b, speedGbps: Math.min(a.speedGbps || 0, b.speedGbps || 0) || null, cable: { sn, pn: a.pn || null }, up: a.link && b.link });
  }
  return links;
}

// A verified wiring wins, then the saved choice, then the Mac's own static
// neighbours (they name the Spark port it was configured for), then a guess
// that the UI flags as unverified.
function suggestMapping(ports, candidates, saved = {}, verified = {}) {
  const mapping = {}; const reasons = {};
  const byMac = {}; for (const c of candidates) if (c.mac) byMac[c.mac] = c;
  const used = new Set();
  const take = (key, c, reason) => { mapping[key] = { spark: c.spark, iface: c.iface }; reasons[key] = reason; used.add(c.key); };
  const find = (m) => m && candidates.find((c) => c.spark === m.spark && c.iface === m.iface && !used.has(c.key));
  for (const p of ports) {
    if (!p.iface) continue;
    const v = find(verified[p.key]); if (v) { take(p.key, v, 'verified'); continue; }
    if (saved[p.key] === null) { reasons[p.key] = 'unassigned'; continue; }
    const s = find(saved[p.key]); if (s) { take(p.key, s, 'saved'); continue; }
    const n = (p.neighbours || []).map((x) => byMac[x.lladdr]).find((c) => c && !used.has(c.key));
    if (n) take(p.key, n, 'neighbour');
  }
  for (const p of ports) {
    if (!p.iface || mapping[p.key] || reasons[p.key] === 'unassigned' || !p.portActive) continue;
    const free = candidates.find((c) => !used.has(c.key) && c.link);
    if (free) take(p.key, free, 'guess');
  }
  return { mapping, reasons };
}

function describe(macs, sparks, links, sLinks) {
  const macIds = new Set(links.map((l) => l.mac.id)), sparkIds = new Set(links.map((l) => l.spark.spark));
  if (!links.length) return { preset: 'none', text: 'No RDMA links yet' };
  const m = macIds.size, s = sparkIds.size;
  const macWord = m === 1 ? 'one Mac' : `${m} Macs`, sparkWord = s === 1 ? 'one Spark' : `${s} Sparks`;
  let preset = 'custom';
  if (m === 1 && s === 1 && links.length === 1) preset = 'single';
  else if (m === 1 && s === 1) preset = 'dual';
  else if (m === 1 && s === 2 && sLinks.length) preset = 'ring';
  else if (m === 1 && s === 2) preset = 'star';
  else if (m === 2 && s === 2) preset = sLinks.length ? 'two-macs-ring' : 'two-macs';
  const names = { single: 'Direct', dual: 'Dual port', star: 'Star', ring: 'Ring', 'two-macs': 'Two Macs, two Sparks', 'two-macs-ring': 'Two Macs, two Sparks, Sparks linked', custom: 'Custom' };
  return { preset, name: names[preset], text: `${macWord} ↔ ${sparkWord} · ${links.length} RDMA link${links.length === 1 ? '' : 's'}${sLinks.length ? ` · Sparks linked at ${sLinks[0].speedGbps || '?'}G` : ''}` };
}

function build({ macs, sparks, settings, lastTests = {} }) {
  const ports = macPorts(macs);
  const candidates = candidatePorts(sparks);
  const sLinks = sparkLinks(sparks);
  const registered = (settings && settings.sparks) || [];
  const linuxPeers = [...sparks];
  for (const peer of registered) if (!linuxPeers.some((p) => p.id === peer.id)) linuxPeers.push({ ...peer, reachable: false, ports: [] });
  const linuxLinks = buildLinuxLinks({ peers: linuxPeers, savedLinks: (settings && settings.linuxLinks) || [], lastTests: (settings && settings.lastLinuxTests) || {}, test: (settings && settings.test) || {} });
  const saved = (settings && settings.mapping) || {};
  const verified = (settings && settings.wiringVerified) || {};
  const { mapping, reasons } = suggestMapping(ports, candidates, saved, verified);
  const links = [];
  for (const p of ports) {
    const m = p.iface ? mapping[p.key] : null;
    if (!m) continue;
    const c = candidates.find((x) => x.spark === m.spark && x.iface === m.iface);
    const sp = sparks.find((s) => s.id === m.spark) || {};
    const macRec = macs.find((x) => x.id === p.macId) || {};
    const info = macRec.info || {};
    const expectedStudioLL = eui64(p.mac);
    const sparkNeighbour = (c.neighbours || []).find((n) => n.permanent && n.addr === expectedStudioLL && n.lladdr === p.mac);
    const studioNeighbour = (p.neighbours || []).find((n) => n.addr === c.gid && n.lladdr === c.mac);
    const v = verified[p.key];
    const wiringVerified = v && v.spark === c.spark && v.iface === c.iface ? v : null;
    const lt = lastTests[p.key];
    const identity = driverIdentity(info, p, c, settings);
    const lastTest = lt && lt.spark === c.spark && lt.iface === c.iface && identity && lt.identity === identity ? lt : null;
    const confMac = (info.tools && info.tools.neighboursConf) || '';
    const confSpark = (sp.persist && sp.persist.conf) || '';
    const status = {
      studioAddress: !!p.linkLocal && p.linkLocal === expectedStudioLL,
      studioNeighbour: !!studioNeighbour,
      sparkNeighbour: !!sparkNeighbour,
      sparkGid: c.gidIndex != null,
      portsActive: !!(p.portActive && c.link),
      studioPersisted: !!(info.tools && info.tools.launchDaemon && c.gid && confMac.includes(c.gid)),
      sparkPersisted: !!(sp.persist && sp.persist.service && expectedStudioLL && confSpark.includes(expectedStudioLL)),
      wiringVerified, lastTest
    };
    status.configured = status.studioAddress && status.studioNeighbour && status.sparkNeighbour && status.sparkGid;
    status.ready = status.configured && status.portsActive;
    links.push({ id: p.key, identity, reason: reasons[p.key] || 'saved', mac: { id: p.macId, kind: p.macKind, host: p.macHost, label: p.macLabel }, studio: p, spark: c,
      sparkHost: sp.host || c.host, sparkName: sp.hostname || c.spark, expected: { studioLinkLocal: expectedStudioLL, sparkLinkLocal: c.gid || c.eui64 }, status });
  }
  const unmapped = ports.filter((p) => !p.iface || !mapping[p.key]);
  const usedCand = new Set(links.map((l) => l.spark.key));
  // Spark ports with a live link that no known Mac uses: probably another host —
  // but only once every known Mac has its driver up and no spare active port.
  const macsSettled = macs.length > 0 && macs.every((m) => m.info && m.info.ok && m.info.loaded && m.info.loaded.loaded) && !unmapped.some((p) => p.portActive);
  const orphans = macsSettled ? candidates.filter((c) => !usedCand.has(c.key) && c.link) : [];
  const summary = describe(macs, sparks, links, sLinks);
  return { ports, studioPorts: ports, candidates, sparkLinks: sLinks, linuxLinks, mapping, reasons, links, unmapped, orphans, preset: summary.preset, presetName: summary.name, summary: linuxLinks.length ? `${summary.text} · ${linuxLinks.length} Linux RDMA link${linuxLinks.length === 1 ? '' : 's'}` : summary.text,
    macs: macs.map((m) => ({ id: m.id, kind: m.kind, host: m.host || null, label: m.label, ok: !!(m.info && m.info.ok), error: m.info && m.info.error || null,
      cards: m.info && m.info.ok ? m.info.pci.cards : [], thunderbolt: m.info && m.info.ok ? m.info.thunderbolt : [], loaded: m.info && m.info.ok ? m.info.loaded : null,
      chip: m.info && m.info.ok ? m.info.chip : null, os: m.info && m.info.ok ? m.info.os : null })),
    sparks: sparks.map((s) => ({ id: s.id, kind: s.kind || 'spark', host: s.host, hostname: s.hostname, reachable: !!s.reachable, gpus: s.gpus || [], os: s.os || null, arch: s.arch || null, ports: s.ports || [], error: s.error || null, root: !!s.root, tools: s.tools || {}, peerTools: s.peerTools || [], peerToolHashes: s.peerToolHashes || {}, persist: s.persist || {} })) };
}

module.exports = { build, candidatePorts, sparkLinks, suggestMapping, portKey };
