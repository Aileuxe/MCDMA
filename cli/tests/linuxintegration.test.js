'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('node:child_process');
const { Host } = require('../lib/exec');
const actions = require('../lib/actions');
const { Store } = require('../lib/store');
const { Engine } = require('../lib/engine');

test('adding Linux neighbours retains existing Mac neighbours and updates only matching entries', async () => {
  let script;
  const previous = Host.prototype.sh;
  Host.prototype.sh = async (_command, options) => { script = options.input; return { code:0,out:'configured\n',err:'' }; };
  try { assert.equal((await actions.configureSpark({spark:{id:'example',host:'example.invalid',root:true},entries:[{iface:'eth1',studioLinkLocal:'fe80::2',studioMac:'02:00:00:00:00:02'}]})).ok,true); }
  finally { Host.prototype.sh = previous; }
  const directory = fs.mkdtempSync(path.join(os.tmpdir(),'mcdma-neighbour-merge-'));
  try {
    fs.writeFileSync(path.join(directory,'neighbours.conf'),'eth0 fe80::1 02:00:00:00:00:01\neth1 fe80::2 02:00:00:00:00:03\n');
    const merge = script.slice(script.indexOf('task_neighbours='),script.indexOf('cat > /usr/local/sbin/mcdma-neighbours')).replaceAll('/etc/mcdma',directory);
    const result = spawnSync('/bin/bash',['-c','set -e\n'+merge],{encoding:'utf8'});
    assert.equal(result.status,0,result.stderr);
    assert.deepEqual(fs.readFileSync(path.join(directory,'neighbours.conf'),'utf8').trim().split('\n').sort(),['eth0 fe80::1 02:00:00:00:00:01','eth1 fe80::2 02:00:00:00:00:02']);
  } finally { fs.rmSync(directory,{recursive:true,force:true}); }
});

test('registered Linux peers reuse Mac mapping without changing existing Spark records', () => {
  const directory=fs.mkdtempSync(path.join(os.tmpdir(),'mcdma-linux-store-'));
  const store=new Store(directory),engine=new Engine({store});
  try {
    store.set({sparks:[{id:'spark',host:'spark.example.invalid'},{id:'linux',host:'linux.example.invalid',kind:'linux'}],mapping:{'local:mcrdma1':{spark:'spark',iface:'eth1'}},linuxLinks:[{id:'pair',a:{node:'linux',iface:'eth1'},b:{node:'spark',iface:'eth1'}}]});
    engine.removeSpark('linux');
    assert.deepEqual(store.get().sparks,[{id:'spark',host:'spark.example.invalid'}]);
    assert.equal(store.get().mapping['local:mcrdma1'].spark,'spark');
    assert.deepEqual(store.get().linuxLinks,[]);
  } finally { engine.dispose();fs.rmSync(directory,{recursive:true,force:true}); }
});

test('existing install action builds Linux peers natively instead of copying the Spark archive', async () => {
  const directory=fs.mkdtempSync(path.join(os.tmpdir(),'mcdma-linux-action-'));
  const store=new Store(directory),engine=new Engine({store});
  const installer=require('../lib/linuxinstall');
  const old=installer.installLinuxPeer;let calls=0;
  engine.state.sparks=[{id:'example',host:'example.invalid',kind:'linux'}];
  engine.refresh=async()=>{};
  installer.installLinuxPeer=async({peer})=>{calls++;assert.equal(peer.id,'example');return{ok:true,binary:'/opt/example/verbs-peer'};};
  try {
    const result=await engine.runAction('installSparkPeer',{spark:'example'});
    assert.equal(result.ok,true);assert.equal(calls,1);
    assert.equal(store.get().tools.linuxPeerPaths.example,'/opt/example/verbs-peer');
  } finally {installer.installLinuxPeer=old;engine.dispose();fs.rmSync(directory,{recursive:true,force:true});}
});
