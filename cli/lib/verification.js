'use strict';
const crypto = require('crypto');

function driverIdentity(info, port, peer, settings) {
  const loaded = info.loaded || {}, provider = info.provider || {};
  if (!loaded.loaded || !loaded.uuid || !loaded.version || !provider.present || !provider.sha256) return null;
  return crypto.createHash('sha256').update(JSON.stringify({
    schema: 2, demo: !!info.demo, os: info.os && info.os.build,
    version: loaded.version, uuid: loaded.uuid, provider: provider.sha256,
    mac: port.mac, device: port.rdmaDevice, peer: peer.mac, peerGid: peer.gid,
    peerGidIndex: peer.gidIndex, peerPort: peer.rdmaPort || 1,
    peerBootId: peer.bootId || null, peerTools: peer.peerToolHashes || {},
    payload: settings.test && settings.test.payload, mtu: settings.test && settings.test.mtu,
    arm: settings.test && settings.test.arm,
    knobs: port.registry && port.registry.knobs
  })).digest('hex');
}

function validateTestSettings(test, latency) {
  if (![1024, 4096].includes(test.payload)) throw new Error('Payload must be 1024 or 4096 bytes');
  if (![1024, 4096].includes(test.mtu)) throw new Error('RDMA path MTU must be 1024 or 4096');
  if (!['kernel', 'direct', 'bf64'].includes(test.arm)) throw new Error('Unknown posting mode');
  if (latency && test.iterations !== undefined && test.iterations !== 1000)
    throw new Error('These peer binaries use exactly 1000 samples per operation; set test.iterations to 1000');
}

function validateMode(stderr, arm) {
  if (!['kernel', 'direct', 'bf64'].includes(arm)) throw new Error('Unknown posting mode');
  const lines = stderr.split(/\r?\n/);
  const cq = lines.filter((l) => l.startsWith('MCDMA_CQ_OBSERVER '));
  const expected = arm === 'kernel' ? 'MCDMA_CQ_OBSERVER mapped=0 reason=disabled' : 'MCDMA_CQ_OBSERVER mapped=1 bytes=16384 readonly=1 consume=user';
  if (cq.length !== 1 || cq[0] !== expected || lines.some((l) => l.startsWith('MCDMA_CQ_CONSUME retired')))
    throw new Error('Provider did not uniquely confirm the requested CQ mode');
  const post = lines.filter((l) => l.includes('MCDMA_USER_POST'));
  const queues = post.filter((l) => /^MCDMA_USER_POST qp=[1-9][0-9]* queue_mapped=1 bytes=16384$/.test(l));
  if (arm === 'kernel' ? post.length !== 0 : post.length !== 2 || queues.length !== 1 || !post.includes('MCDMA_USER_POST enabled=1 uar_bytes=16384'))
    throw new Error('Provider did not uniquely confirm the requested posting mode');
  const bf = lines.filter((l) => l.includes('MCDMA_USER_BF'));
  if (arm !== 'bf64') {
    if (bf.length) throw new Error('Unexpected BlueFlame posting');
  } else {
    const pushes = bf.filter((l) => /^MCDMA_USER_BF qp=[1-9][0-9]* bank_bytes=(128|256|512|1024) bytes=64 store=neon$/.test(l));
    if (bf.length !== 2 || pushes.length !== 1 || !bf.includes('MCDMA_USER_BF mode=64 uar_wc=1'))
      throw new Error('Provider did not uniquely confirm BlueFlame-64');
  }
}

function validateTrace(text, payload) {
  const lines = text.trim().split(/\r?\n/);
  if (lines[0] !== 'operation,bytes,sample,completion_ns' || lines.length !== 2001)
    throw new Error('Expected a complete 2000-sample latency trace');
  for (let i = 0; i < 2000; i++) {
    const columns = lines[i + 1].split(',');
    if (columns.length !== 4 || columns[0] !== (i < 1000 ? 'write' : 'read') ||
        columns[1] !== String(payload) || columns[2] !== String(i % 1000) ||
        !/^[1-9][0-9]*$/.test(columns[3]) || !Number.isSafeInteger(Number(columns[3])))
      throw new Error('Malformed or incomplete latency sample');
  }
  return { csv: text, sha256: crypto.createHash('sha256').update(text).digest('hex'), samplesPerOperation: 1000 };
}

async function readTrace(host, line, payload) {
  if (!/^LATENCY_TRACE \/tmp\/mcdma-cx5-latency-[A-Za-z0-9]+$/.test(line || '')) throw new Error('Invalid latency trace path');
  const remote = line.slice('LATENCY_TRACE '.length);
  const result = await host.sh(`cat '${remote}'`, { timeoutMs: 10000 });
  if (result.code !== 0) throw new Error('Cannot retrieve latency samples');
  return { ...validateTrace(result.out, payload), remote };
}
module.exports = { driverIdentity, validateTestSettings, validateMode, validateTrace, readTrace };
