import { createHash } from 'node:crypto';

export const sha256 = bytes => createHash('sha256').update(bytes).digest('hex');
export function validateArtifacts({ status, packageVersion, header, param, commit, payload, image, verification }) {
  const fail = message => { throw new Error(message); };
  if (status.version !== packageVersion || !header.includes(`#define ORBIT_VERSION "${status.version}"`))
    fail('Release, npm and payload versions do not match');
  if (!/^\d{2}\.\d{3}\.\d{3}$/.test(param.contentVersion || '')) fail('Invalid TV content version');
  const tvVersion = param.contentVersion.split('.').map(Number).join('.');
  if (tvVersion !== status.tvAppVersion || param.titleId !== 'PPSA99177') fail('TV app release metadata does not match');
  if (payload.subarray(0, 4).toString('hex') !== '7f454c46') fail('Invalid ELF artifact');
  if (!payload.includes(Buffer.from(`Orbit-Store/${status.version}\0`))) fail('ELF has the wrong version');
  if (image.length < 0x10560 || image.length > 256 * 1024 * 1024 || image.readUInt32LE(0x1055c) !== 0x19540119)
    fail('Invalid or oversized FFPKG artifact');
  if (verification.sourceCommit !== commit || verification.payloadVersion !== status.version ||
      verification.contentVersion !== param.contentVersion || verification.titleId !== param.titleId ||
      verification.payloadSha256 !== sha256(payload) || verification.imageSha256 !== sha256(image))
    fail('Package verification is stale. Run app/tools/verify-package.sh in the app build image.');
  return {
    name: 'Orbit Store for TV', filename: 'PPSA99177.ffpkg', version: tvVersion,
    url: `https://github.com/saawant12/orbit-store-ps5/releases/download/v${status.version}/PPSA99177.ffpkg`,
    checksum: sha256(image), size: image.length,
  };
}
