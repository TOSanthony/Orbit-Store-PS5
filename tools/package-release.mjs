// Stage both programs, their exact source and pinned dependency sources. Never uploads.
import { execFileSync } from 'node:child_process';
import { copyFileSync, cpSync, existsSync, mkdirSync, readFileSync, readdirSync, renameSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { checkRelease } from './release-check.mjs';
import { sha256, validateArtifacts } from './release-artifacts.mjs';
const root = new URL('../', import.meta.url);
const path = relative => fileURLToPath(new URL(relative, root));
const git = (...args) => execFileSync('git', args, { cwd: path('.'), encoding: 'utf8' }).trim();
const json = relative => JSON.parse(readFileSync(path(relative), 'utf8'));
const status = json('release-status.json');
const blockers = checkRelease(status);
const review = process.argv.includes('--review');
if (process.argv.slice(2).some(arg => arg !== '--review')) throw new Error('Only --review is supported.');
if (blockers.length && !review) throw new Error(`Cannot package a public release: ${blockers.join(', ')}`);
if (git('status', '--porcelain')) throw new Error('Commit or stash every change first: the source archive must match the binaries.');
// The exported source uses encrypted inputs. Fail before staging anything if
// those inputs do not match the reviewed catalogue or bundled revision.
execFileSync('python3', ['tools/catalog.py', 'check'], {cwd: path('.'), stdio: 'inherit'});
const versionNotes = readFileSync(path(`docs/${status.version}-release-notes.md`), 'utf8')
  .replace(/^# [^\n]+\n\s*/, '').trim();
if (!versionNotes || /Draft release notes|not published yet/i.test(versionNotes))
  throw new Error('Finalize the versioned release notes before packaging.');
const commit = git('rev-parse', 'HEAD');
const artifact = path('build/orbit_store.elf');
const tvArtifact = path('app/dist/PPSA99177.ffpkg');
const payload = readFileSync(artifact), image = readFileSync(tvArtifact);
const verification = json('app/build/package-verification.json');
const tvFeed = validateArtifacts({status, packageVersion:json('package.json').version,
  header:readFileSync(path('backend/orbit.h'),'utf8'), param:json('app/sce_sys/param.json'),
  commit, payload, image, verification});
for (const file of [artifact, tvArtifact]) {
  if (statSync(file).mtimeMs < Number(git('log', '-1', '--format=%ct')) * 1000)
    throw new Error(`${file} is older than the last commit. Rebuild the release first.`);
}
const upstream = json('tools/release-sources.json');
mkdirSync(path('build/upstream'), {recursive:true});
for (const dep of upstream) {
  if (!/^[A-Za-z0-9._-]+$/.test(dep.name) || !/^[a-f0-9]{64}$/.test(dep.sha256))
    throw new Error('Invalid dependency source manifest');
  const cached = path(`build/upstream/${dep.name}`);
  if (!existsSync(cached)) {
    const response = await fetch(dep.url);
    if (!response.ok) throw new Error(`Could not download ${dep.url}: HTTP ${response.status}`);
    const data = Buffer.from(await response.arrayBuffer());
    if (sha256(data) !== dep.sha256) throw new Error(`Checksum mismatch: ${dep.name}`);
    writeFileSync(`${cached}.part`, data);
    renameSync(`${cached}.part`, cached);
  }
  if (sha256(readFileSync(cached)) !== dep.sha256) throw new Error(`Checksum mismatch: ${dep.name}`);
}
const out = path(`build/${review ? 'review' : 'release'}-${status.version}/`);
rmSync(out, {recursive:true, force:true});
mkdirSync(`${out}licenses`, {recursive:true});
const source = `orbit-store-${status.version}-source.tar.gz`;
git('archive', '--format=tar.gz', `--prefix=orbit-store-${status.version}/`, '-o', `${out}${source}`, commit);
for (const dep of upstream) copyFileSync(path(`build/upstream/${dep.name}`), `${out}${dep.name}`);
for (const name of ['LICENSE','THIRD-PARTY-NOTICES.md','SOURCE-BUNDLE.md','BUILDING.md'])
  copyFileSync(path(name), `${out}${name}`);
cpSync(path('licenses'), `${out}licenses`, {recursive:true});
// Preserve the exact downloaded C source and certificate data used by this build.
mkdirSync(`${out}build-inputs/cjson`, {recursive:true});
for (const name of ['cJSON.c','cJSON.h','LICENSE'])
  copyFileSync(path(`.deps/cjson/${name}`), `${out}build-inputs/cjson/${name}`);
copyFileSync(path('.deps/cacert.pem'), `${out}build-inputs/cacert.pem`);
writeFileSync(`${out}SOURCES.json`, JSON.stringify({version:status.version,tvAppVersion:status.tvAppVersion,
  sourceCommit:commit,sourceArchive:source,upstream},null,2)+'\n');
function filesIn(directory, prefix='') {
  return readdirSync(directory, {withFileTypes:true}).flatMap(entry => entry.isDirectory()
    ? filesIn(`${directory}/${entry.name}`,`${prefix}${entry.name}/`) : [`${prefix}${entry.name}`]).sort();
}
function checksums(names) {
  return names.map(name => `${sha256(readFileSync(`${out}${name}`))}  ${name}\n`).join('');
}
const sourceFiles = filesIn(out);
writeFileSync(`${out}SOURCE-SHA256SUMS`, checksums(sourceFiles));
const sourceBundle = `orbit-store-${status.version}-source-bundle.zip`;
execFileSync('zip', ['-q', `${out}${sourceBundle}`, ...sourceFiles, 'SOURCE-SHA256SUMS'], {cwd:out});
copyFileSync(artifact, `${out}orbit_store.elf`);
copyFileSync(tvArtifact, `${out}PPSA99177.ffpkg`);
writeFileSync(`${out}orbit_store.elf.sha256`, `${sha256(payload)}  orbit_store.elf\n`);
writeFileSync(`${out}PPSA99177.ffpkg.sha256`, `${sha256(image)}  PPSA99177.ffpkg\n`);
writeFileSync(`${out}tv-app.json`, JSON.stringify(tvFeed,null,2)+'\n');
const payloadFeed = {name:'Orbit Store',payloads:[{
  name:'Orbit Store (Beta)',filename:'orbit_store.elf',
  url:`https://github.com/saawant12/orbit-store-ps5/releases/download/v${status.version}/orbit_store.elf`,
  description:'A modern PS5 download manager with a native TV app, resumable downloads and phone controls.',
  version:`v${status.version}`,category:'Utilities',checksum:sha256(payload),
}]};
writeFileSync(`${out}payloads.json`,JSON.stringify(payloadFeed,null,2)+'\n');
writeFileSync(`${out}BUILD-INFO.json`,JSON.stringify({sourceCommit:commit,version:status.version,
  tvAppVersion:status.tvAppVersion,packageVerification:verification,
  binaries:[{name:'orbit_store.elf',size:payload.length,sha256:sha256(payload)},
    {name:'PPSA99177.ffpkg',size:image.length,sha256:sha256(image)}]},null,2)+'\n');
writeFileSync(`${out}RELEASE-NOTES.md`, `# Orbit Store ${status.version} (Beta)

${review ? `UNRELEASED REVIEW BUNDLE. Outstanding gates: ${blockers.join(', ') || 'none'}. Do not publish this review folder.\n\n` : ''}${versionNotes}

## Install the native TV app

Download \`PPSA99177.ffpkg\` and its \`.sha256\` file, verify the checksum, then copy the FFPKG to \`/data/homebrew/\` on your PS5. Let ShadowMountPlus register it and open **Orbit Store** from the **Games row**. Discover opens first. Choose your sources in the native app's setup screen or **App settings → Download sources**.

If an older Orbit service is already running, stop it deliberately before opening the app so its new bundled service can start. The app preserves a newer saved service. The native app version is **${status.tvAppVersion}**, bundled with Orbit service **${status.version}**.

## Use or update the browser version

Download \`orbit_store.elf\` and its checksum, then run it through your payload manager or ELF loader. Open the Orbit shortcut in the **Media tab**, or pair a phone at \`http://<ps5-ip>:34177/\` on the same network.

Existing users can update the service through **App settings → Update / reinstall**, choose **Stop Orbit to restart**, then run the saved \`/data/orbit-store/orbit_store.elf\` or a synced manager copy. Loading a replacement while Orbit is running saves it for the next start. Pairing, source choices and the queue remain saved.

In the browser, **App settings → TV app** installs the native app from the official feed. The native app has separate TV app and download service controls under **App settings → Updates**. Installation and service restart require your choice. After updating the TV app, close and reopen it. If ShadowMountPlus still opens an older app, restart the console when convenient and run your jailbreak again.

## Downloads and source

- \`orbit_store.elf\` and \`orbit_store.elf.sha256\`: download service and browser interface.
- \`PPSA99177.ffpkg\` and \`PPSA99177.ffpkg.sha256\`: native TV app, including the same ELF.
- \`${sourceBundle}\`: complete application source, corresponding dependency sources, licence notices and rebuild instructions for both programs.

Run \`shasum -a 256 -c <filename>.sha256\` beside each binary to verify it. Source-bundle contents have their own \`SOURCE-SHA256SUMS\`.

Orbit Store is in **beta**. The game catalogue offers single-file FFPFSC, exFAT and FPKG options. FPKG downloads save a \`.pkg\` file; install downloaded game packages separately. The native app FFPKG is Orbit itself. Keep the PS5 awake during downloads. Library actions require a compatible ShadowMount v1 local API. See the [guides](https://github.com/saawant12/orbit-store-ps5/tree/main/guides) for setup and troubleshooting.

Orbit Store is free software under GPL-3.0-or-later. The application source is built from commit \`${commit}\`; component sources and licences are listed in \`THIRD-PARTY-NOTICES.md\` and \`SOURCES.json\`.
`);
writeFileSync(`${out}SHA256SUMS`,checksums(filesIn(out)));
console.log(`Staged ${out} from ${commit}. Nothing was uploaded. Publish the two binaries, their checksums, the source bundle and SHA256SUMS together; update both feeds only after verifying the release assets.`);
