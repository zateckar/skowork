// Small shared pieces of the admin console views.
import { useCallback, useEffect, useState, type FormEvent, type ReactNode } from 'react';
import { api, ApiError } from './auth.ts';

/** GET `path` (null: nothing to load); `reload` fetches again. */
export function useLoad<T>(path: string | null): { data: T | null; error: string; reload: () => void } {
  const [data, setData] = useState<T | null>(null);
  const [error, setError] = useState('');
  const reload = useCallback(() => {
    if (!path) return setData(null);
    api<T>('GET', path).then(
      (d) => (setData(d), setError('')),
      (e: Error) => (setData(null), setError(e.message)),
    );
  }, [path]);
  useEffect(reload, [reload]);
  return { data, error, reload };
}

/** Runs an action and keeps its error (or a success note) for display. */
export function useAction(): { error: ApiError | Error | null; note: string; run: (fn: () => Promise<unknown>, note?: string) => Promise<boolean> } {
  const [error, setError] = useState<ApiError | Error | null>(null);
  const [note, setNote] = useState('');
  const run = useCallback(async (fn: () => Promise<unknown>, done = '') => {
    setNote('');
    try {
      await fn();
      setError(null);
      setNote(done);
      return true;
    } catch (e) {
      setError(e as Error);
      setNote('');
      return false;
    }
  }, []);
  return { error, note, run };
}

// One notice at a time, fixed near the top of the window, so an outcome is visible wherever the admin scrolled (D2).
interface Notice { text: string; kind: 'ok' | 'err'; id?: string; owner: object }
let notice: Notice | null = null;
const listeners = new Set<() => void>();
const setNotice = (n: Notice | null) => ((notice = n), listeners.forEach((l) => l()));

function usePublish(text: string, kind: Notice['kind'], id: string | undefined) {
  const [owner] = useState(() => ({}));
  useEffect(() => {
    if (text) setNotice({ text, kind, id, owner });
    else if (notice?.owner === owner) setNotice(null);
  }, [text, kind, id, owner]);
  useEffect(() => () => void (notice?.owner === owner && setNotice(null)), [owner]);
}

export function Notices() {
  const [, tick] = useState(0);
  useEffect(() => {
    const l = () => tick((n) => n + 1);
    listeners.add(l);
    return () => void listeners.delete(l);
  }, []);
  if (!notice) return null;
  return (
    <div className={`toast ${notice.kind}`} role={notice.kind === 'err' ? 'alert' : 'status'} data-testid={notice.id}>
      <p>{notice.text}</p>
      <button aria-label="Dismiss" title="Dismiss" onClick={() => setNotice(null)}>×</button>
    </div>
  );
}

/** An error next to where it happened; action errors (not load errors) are also raised as a notice. */
export function Err({ error, id }: { error: string | Error | null | undefined; id?: string }) {
  usePublish(error instanceof Error ? error.message : '', 'err', id && `${id}-notice`);
  if (!error) return null;
  return <p className="err" data-testid={id} role="alert">{typeof error === 'string' ? error : error.message}</p>;
}

/** A success note, shown as the notice. */
export function Note({ text, id }: { text: string; id?: string }) {
  usePublish(text, 'ok', id);
  return null;
}

export function Card({ title, id, children, actions }: { title: string; id?: string; children: ReactNode; actions?: ReactNode }) {
  return (
    <section className="card" data-testid={id}>
      <div className="card-head"><h2>{title}</h2>{actions}</div>
      {children}
    </section>
  );
}

/** A control with its visible label above it. */
export function Field({ label, children, grow }: { label: string; children: ReactNode; grow?: boolean }) {
  return <label className={grow ? 'field grow' : 'field'}><span>{label}</span>{children}</label>;
}

/** Asks before a destructive action; false when the admin cancels. */
export const confirmed = (question: string) => window.confirm(question);

/** A form whose submit receives its fields as strings. */
/** With clearOnSuccess, the fields are cleared when onSubmit resolves to true (an add form after the item was created). */
export function Form({ onSubmit, children, id, clearOnSuccess }: { onSubmit: (f: Record<string, string>) => unknown; children: ReactNode; id?: string; clearOnSuccess?: boolean }) {
  const submit = async (e: FormEvent<HTMLFormElement>) => {
    e.preventDefault();
    const form = e.currentTarget;
    const f = Object.fromEntries([...new FormData(form)].map(([k, v]) => [k, String(v)]));
    if ((await onSubmit(f)) === true && clearOnSuccess) form.reset();
  };
  return <form className="inline" onSubmit={submit} data-testid={id}>{children}</form>;
}

export const time = (s: string | null | undefined) => (s ? new Date(s).toLocaleString() : '');
export const show = (v: unknown) => (typeof v === 'string' ? v : JSON.stringify(v));
export const qs = (o: Record<string, string | number | boolean | undefined | null>) =>
  new URLSearchParams(Object.entries(o).filter(([, v]) => v !== undefined && v !== null && v !== '').map(([k, v]) => [k, String(v)])).toString();
