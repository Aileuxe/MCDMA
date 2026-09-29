'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const { parse, PROBE } = require('../lib/sparks');
function fixture(gids, addresses = 'fe80::11:22ff:fe33:4455') {
  return `===ID
example-linux
1000
PRETTY_NAME="Example Linux"
ID=example
x86_64
===LINKS
2: eth9: <UP> link/ether 02:11:22:33:44:55 brd ff:ff:ff:ff:ff:ff
===RDMA
link mlx5_3/2 state ACTIVE physical_state LINK_UP netdev eth9
===GPU
AMD GPU card0
===BOOT
00000000-0000-0000-0000-000000000001
===TOOLS
PEER=/opt/example/verbs-peer
PEER_HASH ${'1'.repeat(64)} /opt/example/verbs-peer
===PORT eth9
Speed: 100000Mb/s
Link detected: yes
---ADDR
${addresses.split(',').map((a) => `inet6 ${a}/64 scope link`).join('\n')}
---DEV mlx5_3 2
${gids}
`;
}
test('Linux discovery retains AMD identity, real port and GID above index five', () => {
  const result = parse(fixture('GID 7 fe80::11:22ff:fe33:4455 RoCE v2 eth9\nGID 11 0000:0000:0000:0000:0000:ffff:c000:0201 RoCE v2 eth9'));
  assert.deepEqual(result.gpus, ['AMD GPU card0']);
  assert.equal(result.arch, 'x86_64');
  assert.equal(result.ports[0].rdmaPort, 2);
  assert.equal(result.ports[0].gidIndex, 7);
  assert.equal(result.peerToolHashes['/opt/example/verbs-peer'], '1'.repeat(64));
});
test('IPv4 and another netdev GID cannot substitute for observed link-local addressing', () => {
  const result = parse(fixture('GID 3 0000:0000:0000:0000:0000:ffff:c000:0201 RoCE v2 eth9\nGID 7 fe80::11:22ff:fe33:4455 RoCE v2 eth8'));
  assert.equal(result.ports[0].gidIndex, null);
});
test('multiple unpreferred link-local addresses fail closed', () => {
  const result = parse(fixture('GID 7 fe80::1 RoCE v2 eth9\nGID 9 fe80::2 RoCE v2 eth9', 'fe80::1,fe80::2'));
  assert.equal(result.ports[0].gid, null);
});
test('probe enumerates each actual port and every GID file', () => {
  assert.match(PROBE, /ports\/"\$port"\/gids\/\*/);
  assert.match(PROBE, /gid_attrs\/ndevs/);
  assert.doesNotMatch(PROBE, /for g in 0 1 2 3 4 5/);
});
