#!/usr/bin/env node
import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { basename, resolve } from 'node:path';
import { spawn } from 'node:child_process';

const buttons = new Set(['a', 'b', 'select', 'start', 'right', 'left', 'up', 'down', 'r', 'l']);

const defaultOutputDir = 'wasm-replay-output';

function usage() {
  console.error('usage: node tools/wasm_replay.mjs <events.txt> [output-dir] [--no-build] [--keep-browser]');
  console.error('event frame numbers are emulated game frames, not display frames');
  console.error('events: screenshot [name], button <name> <on|off>, probe hblank-dma-win0h');
  process.exit(2);
}

function parseArgs(argv) {
  const options = { build: true, keepBrowser: false };
  const paths = [];
  for (const arg of argv) {
    if (arg === '--no-build') options.build = false;
    else if (arg === '--keep-browser') options.keepBrowser = true;
    else paths.push(arg);
  }
  if (paths.length < 1 || paths.length > 2) usage();
  return { inputPath: resolve(paths[0]), outputDir: resolve(paths[1] || defaultOutputDir), options };
}

function parseEvents(text) {
  const events = [];
  const lines = text.split(/\r?\n/);
  for (let index = 0; index < lines.length; index++) {
    const raw = lines[index];
    const line = raw.replace(/#.*/, '').trim();
    if (!line) continue;

    const fields = line.split(/\s+/);
    const frame = Number(fields[0]);
    if (!Number.isInteger(frame) || frame < 0) throw new Error(`${index + 1}: frame must be a non-negative integer`);

    if (fields[1] === 'screenshot') {
      events.push({ frame, type: 'screenshot', name: fields[2] || `frame-${frame}` });
      continue;
    }

    if (fields[1] === 'probe') {
      if (fields.length !== 3 || fields[2] !== 'hblank-dma-win0h') {
        throw new Error(`${index + 1}: expected "<frame> probe hblank-dma-win0h"`);
      }
      events.push({ frame, type: 'probe', name: fields[2] });
      continue;
    }

    if (fields[1] !== 'button' || fields.length !== 4) {
      throw new Error(`${index + 1}: expected "<frame> button <name> <on|off>", "<frame> screenshot [name]", or "<frame> probe hblank-dma-win0h"`);
    }
    const name = fields[2];
    const state = fields[3];
    if (!buttons.has(name)) throw new Error(`${index + 1}: unknown button "${name}"`);
    if (state !== 'on' && state !== 'off') throw new Error(`${index + 1}: button state must be on or off`);
    events.push({ frame, type: 'button', name, pressed: state === 'on' });
  }
  return events.sort((a, b) => a.frame - b.frame || (a.type === 'button' ? -1 : 1));
}

function run(command, args, log, options = {}) {
  return new Promise((resolveRun, reject) => {
    const child = spawn(command, args, { cwd: resolve('.'), env: process.env, ...options });
    child.stdout.on('data', (chunk) => log(`${command} stdout: ${chunk}`));
    child.stderr.on('data', (chunk) => log(`${command} stderr: ${chunk}`));
    child.on('error', reject);
    child.on('exit', (code) => {
      if (code === 0) resolveRun();
      else reject(new Error(`${command} ${args.join(' ')} exited with ${code}`));
    });
  });
}

function browserPath() {
  const candidates = [
    process.env.CHROME_BIN,
    '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
    '/Applications/Chromium.app/Contents/MacOS/Chromium',
    '/usr/bin/google-chrome',
    '/usr/bin/chromium',
    '/usr/bin/chromium-browser',
    '/snap/bin/chromium',
  ].filter(Boolean);
  const found = candidates.find((path) => existsSync(path));
  if (!found) throw new Error('Chrome/Chromium not found; set CHROME_BIN to a browser executable');
  return found;
}

async function startServer(log) {
  const child = spawn('node', ['web/server.mjs'], { cwd: resolve('.'), env: { ...process.env, PORT: '0' } });
  return await new Promise((resolveStart, reject) => {
    const timeout = setTimeout(() => reject(new Error('timed out waiting for wasm web server')), 10000);
    child.stdout.on('data', (chunk) => {
      const text = String(chunk);
      log(`server stdout: ${text}`);
      const match = text.match(/http:\/\/localhost:(\d+)/);
      if (match) {
        clearTimeout(timeout);
        resolveStart({ child, url: `http://localhost:${match[1]}` });
      }
    });
    child.stderr.on('data', (chunk) => log(`server stderr: ${chunk}`));
    child.on('error', reject);
    child.on('exit', (code) => reject(new Error(`server exited before ready with ${code}`)));
  });
}

async function startBrowser(userDataDir, log) {
  const child = spawn(browserPath(), [
    '--headless=new',
    '--remote-debugging-port=0',
    `--user-data-dir=${userDataDir}`,
    '--disable-background-timer-throttling',
    '--disable-renderer-backgrounding',
    '--disable-gpu',
    'about:blank',
  ]);
  return await new Promise((resolveStart, reject) => {
    const timeout = setTimeout(() => reject(new Error('timed out waiting for Chrome DevTools')), 10000);
    child.stderr.on('data', (chunk) => {
      const text = String(chunk);
      log(`browser stderr: ${text}`);
      const match = text.match(/DevTools listening on (ws:\/\/[^\s]+)/);
      if (match) {
        clearTimeout(timeout);
        resolveStart({ child, browserWs: match[1] });
      }
    });
    child.stdout.on('data', (chunk) => log(`browser stdout: ${chunk}`));
    child.on('error', reject);
    child.on('exit', (code) => reject(new Error(`browser exited before ready with ${code}`)));
  });
}

class Cdp {
  constructor(url) {
    this.nextId = 1;
    this.pending = new Map();
    this.handlers = new Map();
    this.socket = new WebSocket(url);
    this.ready = new Promise((resolveReady, reject) => {
      this.socket.addEventListener('open', resolveReady, { once: true });
      this.socket.addEventListener('error', reject, { once: true });
    });
    this.socket.addEventListener('message', (event) => this.receive(JSON.parse(event.data)));
  }

  receive(message) {
    if (message.id) {
      const pending = this.pending.get(message.id);
      if (!pending) return;
      this.pending.delete(message.id);
      if (message.error) pending.reject(new Error(message.error.message));
      else pending.resolve(message.result);
      return;
    }
    const handler = this.handlers.get(message.method);
    if (handler) handler(message.params);
  }

  on(method, handler) {
    this.handlers.set(method, handler);
  }

  async send(method, params = {}) {
    await this.ready;
    const id = this.nextId++;
    this.socket.send(JSON.stringify({ id, method, params }));
    return await new Promise((resolveSend, reject) => this.pending.set(id, { resolve: resolveSend, reject }));
  }

  close() {
    this.socket.close();
  }
}

async function newPage(browserWs) {
  const endpoint = new URL(browserWs);
  const response = await fetch(`http://${endpoint.host}/json/new`, { method: 'PUT' });
  const target = await response.json();
  return new Cdp(target.webSocketDebuggerUrl);
}

async function evaluate(cdp, expression, awaitPromise = true) {
  const result = await cdp.send('Runtime.evaluate', { expression, awaitPromise, returnByValue: true });
  if (result.exceptionDetails) {
    const details = result.exceptionDetails;
    const exception = details.exception?.description || details.exception?.value;
    const location = `${details.url || '<anonymous>'}:${details.lineNumber ?? 0}:${details.columnNumber ?? 0}`;
    throw new Error([details.text, exception, location].filter(Boolean).join('\n'));
  }
  return result.result.value;
}

function safeName(name) {
  return name.replace(/[^A-Za-z0-9._-]+/g, '_');
}

async function saveScreenshot(cdp, outputDir, event) {
  const dataUrl = await evaluate(cdp, `window.pokeemerald.automation.screenshot()`);
  const state = await evaluate(cdp, `window.pokeemerald.automation.state()`);
  const png = Buffer.from(dataUrl.replace(/^data:image\/png;base64,/, ''), 'base64');
  const file = `${String(event.frame).padStart(6, '0')}-${safeName(event.name)}.png`;
  await writeFile(resolve(outputDir, 'screenshots', file), png);
  return { file, state };
}

async function runProbe(cdp, event) {
  const result = await evaluate(cdp, `window.pokeemerald.automation.hblankDmaWin0HProbe()`);
  const stoppedControls = { fixed: 0x0040, reload: 0x0060 };
  for (const mode of ['fixed', 'reload']) {
    const actual = result[mode];
    if (actual?.activeLine0 !== 0
        || actual.activeLine1 !== 63
        || actual.activeLine2 !== 0
        || actual.activeCached !== true
        || actual.stoppedControl !== stoppedControls[mode]
        || actual.stoppedLine0 !== 0
        || actual.stoppedLine1 !== 0
        || actual.stoppedLine2 !== 0
        || actual.stoppedCached !== false) {
      throw new Error(`unexpected ${event.name} ${mode} probe result: ${JSON.stringify(result)}`);
    }
  }
  return { frame: event.frame, name: event.name, result };
}

async function main() {
  const { inputPath, outputDir, options } = parseArgs(process.argv.slice(2));
  await rm(outputDir, { recursive: true, force: true });
  await mkdir(resolve(outputDir, 'screenshots'), { recursive: true });
  const logLines = [];
  const errors = [];
  const log = (line) => logLines.push(String(line).trimEnd());
  const userDataDir = await mkdtemp(resolve(tmpdir(), 'pokeemerald-wasm-replay-'));
  let server;
  let browser;
  let cdp;

  try {
    const events = parseEvents(await readFile(inputPath, 'utf8'));
    await writeFile(resolve(outputDir, 'events.json'), JSON.stringify(events, null, 2));
    if (options.build) await run('make', ['wasm'], log);

    server = await startServer(log);
    browser = await startBrowser(userDataDir, log);
    cdp = await newPage(browser.browserWs);
    cdp.on('Runtime.consoleAPICalled', (params) => log(`console ${params.type}: ${params.args.map((arg) => arg.value ?? arg.description).join(' ')}`));
    cdp.on('Runtime.exceptionThrown', (params) => errors.push(params.exceptionDetails.text));
    cdp.on('Log.entryAdded', (params) => log(`page ${params.entry.level}: ${params.entry.text}`));
    await cdp.send('Runtime.enable');
    await cdp.send('Log.enable');
    await cdp.send('Page.enable');
    await cdp.send('Page.navigate', { url: `${server.url}/?automate=1` });
    await evaluate(cdp, `new Promise((resolve, reject) => {
      const timeout = setTimeout(() => reject(new Error('timed out waiting for wasm automation')), 30000);
      const check = () => {
        if (window.pokeemerald?.automation?.ready) {
          window.pokeemerald.automation.ready.then(() => {
            clearTimeout(timeout);
            resolve();
          });
        } else {
          setTimeout(check, 20);
        }
      };
      check();
    })`, true);

    const screenshots = [];
    const probes = [];
    for (const event of events) {
      await evaluate(cdp, `window.pokeemerald.automation.runToFrame(${event.frame})`);
      if (event.type === 'button') {
        await evaluate(cdp, `window.pokeemerald.automation.setButton(${JSON.stringify(event.name)}, ${event.pressed})`);
      } else if (event.type === 'screenshot') {
        screenshots.push(await saveScreenshot(cdp, outputDir, event));
      } else if (event.type === 'probe') {
        probes.push(await runProbe(cdp, event));
      }
    }
    await writeFile(resolve(outputDir, 'summary.json'), JSON.stringify({ input: basename(inputPath), frameUnit: 'emulated_game_frame', screenshots, probes, errors }, null, 2));
  } catch (error) {
    errors.push(error.stack || String(error));
    process.exitCode = 1;
  } finally {
    await writeFile(resolve(outputDir, 'console.log'), `${logLines.join('\n')}\n`);
    await writeFile(resolve(outputDir, 'errors.log'), `${errors.join('\n')}\n`);
    if (cdp) cdp.close();
    if (browser && !options.keepBrowser) browser.child.kill();
    if (server) server.child.kill();
    if (!options.keepBrowser) {
      try {
        await rm(userDataDir, { recursive: true, force: true });
      } catch {
        // Chrome can leave profile files behind briefly after shutdown.
      }
    }
  }
}

main();
