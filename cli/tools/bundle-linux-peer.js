#!/usr/bin/env node
'use strict';
const fs = require('fs');
const path = require('path');
const { peerSources, validateSources } = require('../lib/linuxinstall');
const root = path.resolve(__dirname, '..', '..');
for (const file of ['peer/verbs_peer.c', 'include/cx5_device.h']) {
  if (!fs.existsSync(path.join(root, file))) throw new Error('Refresh the Linux peer bundle from the MCDMA source checkout');
}
const bundle = peerSources(root);
validateSources(bundle);
bundle.license = 'Apache-2.0';
bundle.provenance = 'Canonical MCDMA peer and device guard sources; refresh together when either source changes.';
fs.writeFileSync(path.join(root, 'cli', 'native-peer.json'), JSON.stringify(bundle, null, 2) + '\n');
