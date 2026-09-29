'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const { peerSources, validateSources, installScript, installLinuxPeer } = require('../lib/linuxinstall');
const hash = (value) => crypto.createHash('sha256').update(value).digest('hex');
test('standalone source bundle matches canonical peer and guard sources', () => {
  const bundle = JSON.parse(fs.readFileSync(path.join(__dirname, '..', 'native-peer.json'), 'utf8'));
  assert.equal(validateSources(bundle), validateSources(peerSources()));
});
test('corrupt source cannot be transferred for compilation', () => {
  const bundle = peerSources(); bundle.files[0].source += '\n';
  assert.throws(() => installScript(bundle), /checksum mismatch/);
});
test('native install is user-scoped and requires the stock Linux ABI marker', () => {
  const script = installScript(peerSources());
  assert.match(script, /\$HOME\/\.local\/libexec\/mcdma/);
  assert.match(script, /stock_initiator=1/);
  assert.match(script, /verbs-peer\.previous-/);
  assert.doesNotMatch(script, /sudo|modprobe|insmod|apt-get/);
});
test('installation reports success only for a verified native build', async () => {
  const host = { sh: async () => ({code:0,out:`MCDMA_LINUX_PEER_INSTALLED\nPATH /opt/example/verbs-peer\nSHA256 ${hash('example')}\nARCH x86_64\n`,err:''}) };
  const result = await installLinuxPeer({peer:{id:'example',host:'example.invalid'},host});
  assert.equal(result.ok,true);
  assert.equal(result.arch,'x86_64');
  host.sh = async () => ({code:1,out:'',err:'incompatible tool'});
  assert.equal((await installLinuxPeer({peer:{id:'example',host:'example.invalid'},host})).ok,false);
});
