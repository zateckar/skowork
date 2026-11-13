// The app host of client/local-apps (add-local-apps design D2), loaded with --require before a Local app's entry.
// The app calls http.createServer(...).listen(process.env.SKOWORK_APP_PIPE) as usual; that listen is served over
// frames on stdin and stdout instead of a socket, so the app opens no socket or pipe (the runtime refuses the network
// without --allow-net) and only the client reaches it. The app's console output becomes 'log' frames.
//
// Frame: u32le header length, u32le body length, the JSON header {t, id}, the body.
//   client -> app: open {id}, data {id} + bytes, end {id}
//   app -> client: ready, data {id} + bytes, end {id}, log + text
'use strict';
const net = require('node:net');
const fs = require('node:fs');
const { Duplex } = require('node:stream');

const target = process.env.SKOWORK_APP_PIPE;
const MAX_CHUNK = 1 << 20;

function writeRaw(b) {
  let o = 0;
  while (o < b.length) {
    try {
      o += fs.writeSync(1, b, o);
    } catch (e) {
      if (e.code !== 'EAGAIN') throw e;
    }
  }
}

function frame(t, id, body) {
  const h = Buffer.from(JSON.stringify(id === undefined ? { t } : { t, id }));
  const b = body === undefined ? Buffer.alloc(0) : Buffer.isBuffer(body) ? body : Buffer.from(String(body));
  for (let at = 0; at === 0 || at < b.length; at += MAX_CHUNK) {
    const part = b.subarray(at, Math.min(b.length, at + MAX_CHUNK));
    const p = Buffer.alloc(8);
    p.writeUInt32LE(h.length, 0);
    p.writeUInt32LE(part.length, 4);
    writeRaw(Buffer.concat([p, h, part]));
    if (b.length === 0) break;
  }
}

// Console output goes to the app's log, never into the frame stream.
const toLog = (chunk, enc, cb) => {
  frame('log', undefined, Buffer.isBuffer(chunk) ? chunk : Buffer.from(String(chunk), typeof enc === 'string' ? enc : 'utf8'));
  if (typeof enc === 'function') enc();
  else if (typeof cb === 'function') cb();
  return true;
};
process.stdout.write = toLog;
process.stderr.write = toLog;

let served = false;
const listen = net.Server.prototype.listen;
net.Server.prototype.listen = function (...args) {
  const a = args[0];
  const path = typeof a === 'string' ? a : a && typeof a === 'object' ? a.path : undefined;
  if (!target || path !== target) return listen.apply(this, args);
  if (served) throw new Error('Only one server can listen on SKOWORK_APP_PIPE.');
  served = true;
  const server = this;
  const done = args.find((x) => typeof x === 'function');
  const conns = new Map();
  let buf = Buffer.alloc(0);
  process.stdin.on('data', (d) => {
    buf = buf.length ? Buffer.concat([buf, d]) : d;
    while (buf.length >= 8) {
      const hl = buf.readUInt32LE(0), bl = buf.readUInt32LE(4);
      if (buf.length < 8 + hl + bl) break;
      const h = JSON.parse(buf.subarray(8, 8 + hl).toString('utf8'));
      const body = Buffer.from(buf.subarray(8 + hl, 8 + hl + bl));
      buf = buf.subarray(8 + hl + bl);
      if (h.t === 'open') {
        const id = h.id;
        const sock = new Duplex({
          read() {},
          write(chunk, enc, cb) {
            frame('data', id, chunk);
            cb();
          },
          final(cb) {
            if (conns.delete(id)) frame('end', id);
            cb();
          },
          destroy(err, cb) {
            if (conns.delete(id)) frame('end', id);
            cb(err);
          },
        });
        sock.remoteAddress = '127.0.0.1';
        sock.remotePort = 0;
        sock.encrypted = false;
        sock.setTimeout = () => sock;
        sock.setNoDelay = () => sock;
        sock.setKeepAlive = () => sock;
        sock.on('error', () => {});
        conns.set(id, sock);
        server.emit('connection', sock);
      } else if (h.t === 'data') {
        conns.get(h.id)?.push(body);
      } else if (h.t === 'end') {
        conns.get(h.id)?.push(null);
      }
    }
  });
  process.stdin.on('end', () => process.exit(0));
  setImmediate(() => {
    server.emit('listening');
    if (done) done();
    frame('ready');
  });
  return this;
};
