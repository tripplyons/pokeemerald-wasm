import { createReadStream, existsSync, statSync } from 'node:fs';
import { extname, join, normalize, resolve } from 'node:path';
import { createServer } from 'node:http';

const root = resolve(process.cwd());
const port = Number(process.env.PORT || 8000);
const types = new Map([
  ['.html', 'text/html; charset=utf-8'],
  ['.js', 'text/javascript; charset=utf-8'],
  ['.css', 'text/css; charset=utf-8'],
  ['.wasm', 'application/wasm'],
]);

function fileFor(url) {
  const pathname = new URL(url, `http://localhost:${port}`).pathname;
  const relative = pathname === '/' ? 'web/index.html' : pathname.slice(1);
  const file = resolve(root, normalize(relative));
  return file.startsWith(root) ? file : null;
}

createServer((req, res) => {
  const file = fileFor(req.url);
  if (!file || !existsSync(file) || !statSync(file).isFile()) {
    res.writeHead(404).end('not found');
    return;
  }

  res.writeHead(200, {
    'Content-Type': types.get(extname(file)) || 'application/octet-stream',
    'Cache-Control': 'no-store',
  });
  createReadStream(file).pipe(res);
}).listen(port, () => {
  console.log(`pokeemerald wasm server: http://localhost:${port}`);
});
