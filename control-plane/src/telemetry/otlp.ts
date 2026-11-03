// OTLP/HTTP protobuf for log records (add-telemetry-and-admin design D1): the subset of ExportLogsServiceRequest the
// client sends (resource attributes, one scope, log records with time, event name and attributes), decoded for the
// ingest and encoded again for forwarding. Field numbers from opentelemetry/proto (collector/logs/v1, logs/v1,
// resource/v1, common/v1).

export type Value = string | number | boolean | bigint;

export interface LogRecord {
  timeUnixNano: bigint;
  eventName: string;
  attributes: Map<string, Value>;
}

export interface LogBatch {
  resource: Map<string, Value>;
  scopeName: string;
  scopeVersion: string;
  records: LogRecord[];
}

export class OtlpError extends Error {}

// ---- Reading ---------------------------------------------------------------------------------------------------------

const VARINT = 0;
const I64 = 1;
const LEN = 2;
const I32 = 5;

interface FieldValue {
  field: number;
  wire: number;
  num: bigint;
  bytes: Uint8Array;
}

function* fields(buf: Uint8Array): Generator<FieldValue> {
  let i = 0;
  const varint = (): bigint => {
    let v = 0n;
    for (let shift = 0n; shift < 70n; shift += 7n) {
      if (i >= buf.length) throw new OtlpError('truncated varint');
      const c = buf[i++]!;
      v |= BigInt(c & 0x7f) << shift;
      if (!(c & 0x80)) return v;
    }
    throw new OtlpError('varint too long');
  };
  const fixed = (n: number): bigint => {
    if (buf.length - i < n) throw new OtlpError('truncated fixed');
    let v = 0n;
    for (let k = 0; k < n; k++) v |= BigInt(buf[i + k]!) << BigInt(8 * k);
    i += n;
    return v;
  };
  while (i < buf.length) {
    const tag = varint();
    const field = Number(tag >> 3n);
    const wire = Number(tag & 7n);
    if (field === 0) throw new OtlpError('field 0');
    if (wire === VARINT) yield { field, wire, num: varint(), bytes: EMPTY };
    else if (wire === I64) yield { field, wire, num: fixed(8), bytes: EMPTY };
    else if (wire === I32) yield { field, wire, num: fixed(4), bytes: EMPTY };
    else if (wire === LEN) {
      const n = Number(varint());
      if (n > buf.length - i) throw new OtlpError('truncated bytes');
      yield { field, wire, num: 0n, bytes: buf.subarray(i, i + n) };
      i += n;
    } else throw new OtlpError(`wire type ${wire}`);
  }
}

const EMPTY = new Uint8Array(0);
const utf8 = new TextDecoder('utf-8', { fatal: true });
const text = (b: Uint8Array): string => {
  try {
    return utf8.decode(b);
  } catch {
    throw new OtlpError('invalid UTF-8');
  }
};

function anyValue(b: Uint8Array): Value {
  let v: Value = '';
  for (const f of fields(b)) {
    if (f.field === 1 && f.wire === LEN) v = text(f.bytes);
    else if (f.field === 2 && f.wire === VARINT) v = f.num !== 0n;
    else if (f.field === 3 && f.wire === VARINT) v = BigInt.asIntN(64, f.num);
    else if (f.field === 4 && f.wire === I64) {
      const dv = new DataView(new ArrayBuffer(8));
      dv.setBigUint64(0, f.num, true);
      v = dv.getFloat64(0, true);
    }
  }
  return v;
}

function keyValue(b: Uint8Array): [string, Value] {
  let key = '';
  let value: Value = '';
  for (const f of fields(b)) {
    if (f.field === 1 && f.wire === LEN) key = text(f.bytes);
    else if (f.field === 2 && f.wire === LEN) value = anyValue(f.bytes);
  }
  return [key, value];
}

/** Decodes an ExportLogsServiceRequest. Throws OtlpError for malformed input or more than one resource. */
export function decodeLogs(buf: Uint8Array, limits = { maxRecords: 1000, maxAttributes: 64 }): LogBatch {
  const batch: LogBatch = { resource: new Map(), scopeName: '', scopeVersion: '', records: [] };
  let resources = 0;
  for (const rl of fields(buf)) {
    if (rl.field !== 1 || rl.wire !== LEN) continue;
    if (++resources > 1) throw new OtlpError('one resource per batch');
    for (const f of fields(rl.bytes)) {
      if (f.field === 1 && f.wire === LEN) {
        for (const a of fields(f.bytes)) if (a.field === 1 && a.wire === LEN) batch.resource.set(...keyValue(a.bytes));
      } else if (f.field === 2 && f.wire === LEN) {
        for (const s of fields(f.bytes)) {
          if (s.field === 1 && s.wire === LEN) {
            for (const sc of fields(s.bytes)) {
              if (sc.field === 1 && sc.wire === LEN) batch.scopeName = text(sc.bytes);
              if (sc.field === 2 && sc.wire === LEN) batch.scopeVersion = text(sc.bytes);
            }
          } else if (s.field === 2 && s.wire === LEN) {
            const rec: LogRecord = { timeUnixNano: 0n, eventName: '', attributes: new Map() };
            for (const r of fields(s.bytes)) {
              if (r.field === 1 && r.wire === I64) rec.timeUnixNano = r.num;
              else if (r.field === 12 && r.wire === LEN) rec.eventName = text(r.bytes);
              else if (r.field === 6 && r.wire === LEN) {
                if (rec.attributes.size >= limits.maxAttributes) throw new OtlpError('too many attributes');
                rec.attributes.set(...keyValue(r.bytes));
              }
            }
            if (batch.records.length >= limits.maxRecords) throw new OtlpError('too many records');
            batch.records.push(rec);
          }
        }
      }
    }
  }
  return batch;
}

// ---- Writing ---------------------------------------------------------------------------------------------------------

class Writer {
  readonly parts: number[] = [];
  varint(v: bigint | number): this {
    let x = BigInt.asUintN(64, BigInt(v));
    while (x >= 0x80n) {
      this.parts.push(Number((x & 0x7fn) | 0x80n));
      x >>= 7n;
    }
    this.parts.push(Number(x));
    return this;
  }
  tag(field: number, wire: number): this {
    return this.varint((field << 3) | wire);
  }
  bytes(field: number, b: Uint8Array): this {
    this.tag(field, LEN).varint(b.length);
    for (const c of b) this.parts.push(c);
    return this;
  }
  string(field: number, s: string): this {
    return this.bytes(field, new TextEncoder().encode(s));
  }
  fixed64(field: number, v: bigint): this {
    this.tag(field, I64);
    for (let k = 0n; k < 8n; k++) this.parts.push(Number((v >> (8n * k)) & 0xffn));
    return this;
  }
  done(): Uint8Array {
    return Uint8Array.from(this.parts);
  }
}

function encodeAny(v: Value): Uint8Array {
  const w = new Writer();
  if (typeof v === 'string') w.string(1, v);
  else if (typeof v === 'boolean') w.tag(2, VARINT).varint(v ? 1 : 0);
  else if (typeof v === 'bigint' || Number.isInteger(v)) w.tag(3, VARINT).varint(BigInt(v));
  else {
    const dv = new DataView(new ArrayBuffer(8));
    dv.setFloat64(0, v, true);
    w.fixed64(4, dv.getBigUint64(0, true));
  }
  return w.done();
}

function encodeKv(k: string, v: Value): Uint8Array {
  return new Writer().string(1, k).bytes(2, encodeAny(v)).done();
}

export function encodeLogs(batch: LogBatch): Uint8Array {
  const res = new Writer();
  for (const [k, v] of batch.resource) res.bytes(1, encodeKv(k, v));
  const scope = new Writer().string(1, batch.scopeName).string(2, batch.scopeVersion).done();
  const sl = new Writer().bytes(1, scope);
  for (const r of batch.records) {
    const lr = new Writer().fixed64(1, r.timeUnixNano).tag(2, VARINT).varint(9);
    for (const [k, v] of r.attributes) lr.bytes(6, encodeKv(k, v));
    lr.fixed64(11, r.timeUnixNano).string(12, r.eventName);
    sl.bytes(2, lr.done());
  }
  const rl = new Writer().bytes(1, res.done()).bytes(2, sl.done()).done();
  return new Writer().bytes(1, rl).done();
}
