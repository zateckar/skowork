// A small zip reader and writer for Skill packages (add-skills design D6). Stored and deflated entries only; no
// encryption, no zip64, no multi-disk archives. Every limit is checked before data is inflated.
import { deflateRawSync, inflateRawSync } from 'node:zlib';

export interface ZipLimits {
  maxEntries: number;
  maxUnpackedBytes: number;
}

export interface ZipEntry {
  path: string;
  data: Buffer;
}

export class ZipError extends Error {}

const EOCD = 0x06054b50;
const CENTRAL = 0x02014b50;
const LOCAL = 0x04034b50;

/** A path that stays inside the archive: relative, forward slashes, no `..`, no drive, no control characters. */
export function safePath(p: string): boolean {
  if (!p || p.length > 260 || p.startsWith('/') || p.includes('\\') || /^[A-Za-z]:/.test(p)) return false;
  // eslint-disable-next-line no-control-regex
  if (/[\u0000-\u001f:*?"<>|]/.test(p)) return false;
  const parts = p.replace(/\/$/, '').split('/');
  return parts.every((s) => s !== '' && s !== '.' && s !== '..' && !/[. ]$/.test(s));
}

export function readZip(buf: Buffer, limits: ZipLimits): ZipEntry[] {
  if (buf.length < 22) throw new ZipError('The package is not a zip archive.');
  let eocd = -1;
  for (let i = buf.length - 22; i >= Math.max(0, buf.length - 22 - 0xffff); i--) {
    if (buf.readUInt32LE(i) === EOCD) {
      eocd = i;
      break;
    }
  }
  if (eocd < 0) throw new ZipError('The package is not a zip archive.');
  if (buf.readUInt16LE(eocd + 4) !== 0 || buf.readUInt16LE(eocd + 6) !== 0) throw new ZipError('Multi-part archives are not supported.');
  const count = buf.readUInt16LE(eocd + 10);
  const cdSize = buf.readUInt32LE(eocd + 12);
  const cdOffset = buf.readUInt32LE(eocd + 16);
  if (count === 0xffff || cdOffset === 0xffffffff) throw new ZipError('Zip64 archives are not supported.');
  if (count > limits.maxEntries) throw new ZipError(`The package has ${count} entries; the limit is ${limits.maxEntries}.`);
  if (cdOffset + cdSize > eocd) throw new ZipError('The zip central directory is damaged.');

  const out: ZipEntry[] = [];
  const seen = new Set<string>();
  let unpacked = 0;
  let p = cdOffset;
  for (let n = 0; n < count; n++) {
    if (p + 46 > eocd || buf.readUInt32LE(p) !== CENTRAL) throw new ZipError('The zip central directory is damaged.');
    const flags = buf.readUInt16LE(p + 8);
    const method = buf.readUInt16LE(p + 10);
    const csize = buf.readUInt32LE(p + 20);
    const usize = buf.readUInt32LE(p + 24);
    const nameLen = buf.readUInt16LE(p + 28);
    const extraLen = buf.readUInt16LE(p + 30);
    const commentLen = buf.readUInt16LE(p + 32);
    const local = buf.readUInt32LE(p + 42);
    const name = buf.subarray(p + 46, p + 46 + nameLen).toString('utf8');
    p += 46 + nameLen + extraLen + commentLen;
    if (flags & 0x1) throw new ZipError(`The entry ${name} is encrypted.`);
    if (!safePath(name)) throw new ZipError(`The entry "${name}" has an unsafe path.`);
    const key = name.replace(/\/$/, '').toLowerCase();
    if (seen.has(key)) throw new ZipError(`The entry ${name} appears twice.`);
    seen.add(key);
    if (name.endsWith('/')) continue;
    unpacked += usize;
    if (unpacked > limits.maxUnpackedBytes) throw new ZipError(`The package unpacks to more than ${limits.maxUnpackedBytes} bytes.`);
    if (local + 30 > buf.length || buf.readUInt32LE(local) !== LOCAL) throw new ZipError(`The entry ${name} is damaged.`);
    const start = local + 30 + buf.readUInt16LE(local + 26) + buf.readUInt16LE(local + 28);
    if (start + csize > buf.length) throw new ZipError(`The entry ${name} is damaged.`);
    const raw = buf.subarray(start, start + csize);
    let data: Buffer;
    if (method === 0) data = Buffer.from(raw);
    else if (method === 8) {
      try {
        data = inflateRawSync(raw, { maxOutputLength: Math.max(usize, 1) });
      } catch {
        throw new ZipError(`The entry ${name} cannot be inflated or is larger than it declares.`);
      }
    } else throw new ZipError(`The entry ${name} uses an unsupported compression method.`);
    if (data.length !== usize) throw new ZipError(`The entry ${name} is damaged.`);
    out.push({ path: name, data });
  }
  return out;
}

/** Writes a deflated zip (tests, and packages the registry assembles). */
export function writeZip(files: Record<string, string | Buffer>): Buffer {
  const locals: Buffer[] = [];
  const centrals: Buffer[] = [];
  let offset = 0;
  for (const [name, content] of Object.entries(files)) {
    const data = Buffer.isBuffer(content) ? content : Buffer.from(content, 'utf8');
    const packed = deflateRawSync(data);
    const nameBuf = Buffer.from(name, 'utf8');
    const crc = crc32(data);
    const lh = Buffer.alloc(30);
    lh.writeUInt32LE(LOCAL, 0);
    lh.writeUInt16LE(20, 4);
    lh.writeUInt16LE(0x800, 6);
    lh.writeUInt16LE(8, 8);
    lh.writeUInt32LE(crc, 14);
    lh.writeUInt32LE(packed.length, 18);
    lh.writeUInt32LE(data.length, 22);
    lh.writeUInt16LE(nameBuf.length, 26);
    const ch = Buffer.alloc(46);
    ch.writeUInt32LE(CENTRAL, 0);
    ch.writeUInt16LE(20, 4);
    ch.writeUInt16LE(20, 6);
    ch.writeUInt16LE(0x800, 8);
    ch.writeUInt16LE(8, 10);
    ch.writeUInt32LE(crc, 16);
    ch.writeUInt32LE(packed.length, 20);
    ch.writeUInt32LE(data.length, 24);
    ch.writeUInt16LE(nameBuf.length, 28);
    ch.writeUInt32LE(offset, 42);
    locals.push(lh, nameBuf, packed);
    centrals.push(ch, nameBuf);
    offset += 30 + nameBuf.length + packed.length;
  }
  const cd = Buffer.concat(centrals);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(EOCD, 0);
  end.writeUInt16LE(Object.keys(files).length, 8);
  end.writeUInt16LE(Object.keys(files).length, 10);
  end.writeUInt32LE(cd.length, 12);
  end.writeUInt32LE(offset, 16);
  return Buffer.concat([...locals, cd, end]);
}

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

function crc32(b: Buffer): number {
  let c = 0xffffffff;
  for (const x of b) c = CRC_TABLE[(c ^ x) & 0xff]! ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}
