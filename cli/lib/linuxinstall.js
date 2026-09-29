'use strict';
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { Host } = require('./exec');
const q = (value) => "'" + String(value).replace(/'/g, "'\\''") + "'";
const FILES = ['peer/verbs_peer.c', 'include/cx5_device.h'];
const hash = (value) => crypto.createHash('sha256').update(value).digest('hex');

function peerSources(root = path.resolve(__dirname, '..', '..')) {
  if (FILES.every((name) => fs.existsSync(path.join(root, name)))) {
    return { schema: 1, files: FILES.map((name) => { const source = fs.readFileSync(path.join(root, name), 'utf8'); return { name, source, sha256: hash(source) }; }) };
  }
  return JSON.parse(fs.readFileSync(path.join(__dirname, '..', 'native-peer.json'), 'utf8'));
}
function validateSources(bundle) {
  if (!bundle || bundle.schema !== 1 || !Array.isArray(bundle.files) || bundle.files.length !== FILES.length) throw new Error('Native peer source bundle is incomplete');
  for (const name of FILES) {
    const matches = bundle.files.filter((f) => f.name === name);
    if (matches.length !== 1 || typeof matches[0].source !== 'string' || hash(matches[0].source) !== matches[0].sha256) throw new Error('Native peer source checksum mismatch');
  }
  return hash(JSON.stringify(bundle.files.map((f) => [f.name, f.sha256]).sort()));
}
function installScript(bundle) {
  const identity = validateSources(bundle);
  const writes = bundle.files.map((file) => `printf '%s' ${q(file.source)} > "$task_build/${file.name}"`).join('\n');
  return `set -eu
umask 077
[ "$(uname -s)" = Linux ] || { echo 'This installer requires Linux' >&2; exit 2; }
case "$(uname -m)" in x86_64|aarch64) ;; *) echo 'Unsupported Linux architecture' >&2; exit 2 ;; esac
command -v cc >/dev/null || { echo 'Install a C compiler and libibverbs development headers first' >&2; exit 3; }
command -v sha256sum >/dev/null || { echo 'sha256sum is required' >&2; exit 3; }
task_build=$(mktemp -d "\${TMPDIR:-/tmp}/mcdma-linux-peer.XXXXXX")
trap 'rm -rf "$task_build"' EXIT
mkdir -p "$task_build/peer" "$task_build/include"
${writes}
cc -std=c11 -O2 -Wall -Wextra -Werror "$task_build/peer/verbs_peer.c" -libverbs -o "$task_build/verbs-peer"
[ "$("$task_build/verbs-peer" --version)" = 'MCDMA_VERBS_PEER abi=1 platform=linux stock_initiator=1 stock_responder=1' ] || { echo 'Peer does not support the stock Linux protocol' >&2; exit 4; }
task_destination="$HOME/.local/libexec/mcdma"
mkdir -p "$task_destination"
if [ -f "$task_destination/verbs-peer" ]; then
  task_previous=$(sha256sum "$task_destination/verbs-peer" | cut -d' ' -f1)
  cp -p "$task_destination/verbs-peer" "$task_destination/verbs-peer.previous-$task_previous"
fi
cp "$task_build/verbs-peer" "$task_destination/verbs-peer.next"
chmod 755 "$task_destination/verbs-peer.next"
mv "$task_destination/verbs-peer.next" "$task_destination/verbs-peer"
printf 'MCDMA_LINUX_PEER_INSTALLED\\nPATH %s\\nSHA256 %s\\nARCH %s\\nSOURCE ${identity}\\n' "$task_destination/verbs-peer" "$(sha256sum "$task_destination/verbs-peer" | cut -d' ' -f1)" "$(uname -m)"
`;
}
async function installLinuxPeer({ peer, root, host = null }) {
  if (!peer || !peer.host) throw new Error('Select a registered Linux peer');
  host = host || new Host('ssh', peer.host);
  const script = installScript(peerSources(root));
  const result = await host.sh('/bin/bash -s', { input: script, timeoutMs: 60000 });
  const binary = (result.out.match(/^PATH (.+)$/m) || [])[1];
  const sha256 = (result.out.match(/^SHA256 ([a-f0-9]{64})$/m) || [])[1];
  const arch = (result.out.match(/^ARCH (x86_64|aarch64)$/m) || [])[1];
  const ok = result.code === 0 && /^MCDMA_LINUX_PEER_INSTALLED$/m.test(result.out) && binary && binary.startsWith('/') && sha256 && arch;
  return { ok: !!ok, binary, sha256, arch, message: ok ? `${peer.id}: native Linux peer installed for ${arch}` : `${peer.id}: ${(result.err || result.out).trim().split('\n').slice(-2).join(' ') || 'native build failed'}` };
}
module.exports = { peerSources, validateSources, installScript, installLinuxPeer };
