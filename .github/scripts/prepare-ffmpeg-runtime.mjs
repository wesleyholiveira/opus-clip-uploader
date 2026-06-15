#!/usr/bin/env node
import { execFileSync } from 'node:child_process';
import { chmodSync, copyFileSync, existsSync, mkdirSync, rmSync, writeFileSync, appendFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { basename, join, resolve } from 'node:path';

const projectRoot = resolve(process.cwd());
const cacheDir = join(projectRoot, '.ffmpeg-runtime-cache');
const runtimeDir = join(projectRoot, '.ffmpeg-runtime');
const executableName = process.platform === 'win32' ? 'ffmpeg.exe' : 'ffmpeg';
const runtimeExecutable = join(runtimeDir, executableName);
const packageJsonPath = join(cacheDir, 'package.json');
function run(command, args, options = {}) {
  execFileSync(command, args, {
    stdio: 'inherit',
    shell: false,
    ...options,
  });
}

function runNpmInstall() {
  const installArgs = ['install', 'ffmpeg-static@5.2.0', '--no-audit', '--no-fund', '--silent'];

  if (process.platform === 'win32') {
    const comspec = process.env.ComSpec || 'cmd.exe';
    run(comspec, ['/d', '/s', '/c', ['npm.cmd', ...installArgs].join(' ')], { cwd: cacheDir });
    return;
  }

  run('npm', installArgs, { cwd: cacheDir });
}

function resolveFfmpegStaticPath() {
  if (!existsSync(packageJsonPath)) {
    return '';
  }

  try {
    const requireFromCache = createRequire(packageJsonPath);
    return String(requireFromCache('ffmpeg-static')).trim();
  } catch {
    return '';
  }
}

rmSync(runtimeDir, { recursive: true, force: true });
mkdirSync(cacheDir, { recursive: true });
mkdirSync(runtimeDir, { recursive: true });

if (!existsSync(packageJsonPath)) {
  writeFileSync(packageJsonPath, JSON.stringify({ private: true, dependencies: {} }, null, 2));
}

let ffmpegStaticPath = resolveFfmpegStaticPath();
if (!ffmpegStaticPath || !existsSync(ffmpegStaticPath)) {
  runNpmInstall();
  ffmpegStaticPath = resolveFfmpegStaticPath();
}

if (!ffmpegStaticPath || basename(ffmpegStaticPath).toLowerCase().indexOf('ffmpeg') === -1) {
  throw new Error(`ffmpeg-static did not return a valid ffmpeg path: ${ffmpegStaticPath}`);
}

if (!existsSync(ffmpegStaticPath)) {
  throw new Error(`ffmpeg-static resolved to a missing file: ${ffmpegStaticPath}`);
}

copyFileSync(ffmpegStaticPath, runtimeExecutable);
chmodSync(runtimeExecutable, 0o755);

console.log(`Prepared FFmpeg runtime executable: ${runtimeExecutable}`);

const githubEnv = process.env.GITHUB_ENV;
if (githubEnv) {
  appendFileSync(githubEnv, `CLIP_CROPPER_FFMPEG_RUNTIME_DIR=${runtimeDir}\n`);
  appendFileSync(githubEnv, `CLIP_CROPPER_FFMPEG_PATH=${runtimeExecutable}\n`);
}
