// Shared pieces of the admin console views (redesign-admin-console-records D1): data hooks, notices, records tables,
// dialogs, the confirmation dialog, pickers and tabs.
import { useCallback, useEffect, useId, useMemo, useRef, useState, type FormEvent, type KeyboardEvent, type ReactNode } from 'react';
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

// ---- notices: one at a time, fixed near the top of the window (improve-admin-console-ux D2) ----------------------

interface Notice { text: string; kind: 'ok' | 'err'; id?: string; owner: object }
let notice: Notice | null = null;
const noticeListeners = new Set<() => void>();
const setNotice = (n: Notice | null) => ((notice = n), noticeListeners.forEach((l) => l()));

/** Shows a success note as the notice; `id` becomes its data-testid. */
export const notify = (text: string, id?: string) => setNotice({ text, kind: 'ok', id, owner: {} });

function usePublish(text: string, kind: Notice['kind'], id: string | undefined) {
  const [owner] = useState(() => ({}));
  useEffect(() => {
    if (text) setNotice({ text, kind, id, owner });
    else if (notice?.owner === owner) setNotice(null);
  }, [text, kind, id, owner]);
  useEffect(() => () => void (notice?.owner === owner && setNotice(null)), [owner]);
}

function useStore<T>(get: () => T, listeners: Set<() => void>): T {
  const [, tick] = useState(0);
  useEffect(() => {
    const l = () => tick((n) => n + 1);
    listeners.add(l);
    return () => void listeners.delete(l);
  }, [listeners]);
  return get();
}

export function Notices() {
  const n = useStore(() => notice, noticeListeners);
  // A row action that fails after its confirmation has no form to show the error in: it becomes the notice.
  useEffect(() => {
    const on = (e: PromiseRejectionEvent) => {
      if (!(e.reason instanceof Error)) return;
      e.preventDefault();
      setNotice({ text: e.reason.message, kind: 'err', id: 'action-error', owner: {} });
    };
    addEventListener('unhandledrejection', on);
    return () => removeEventListener('unhandledrejection', on);
  }, []);
  if (!n) return null;
  return (
    <div className={`toast ${n.kind}`} role={n.kind === 'err' ? 'alert' : 'status'} data-testid={n.id}>
      <p>{n.text}</p>
      <button type="button" aria-label="Dismiss" title="Dismiss" onClick={() => setNotice(null)}>×</button>
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

// ---- layout ---------------------------------------------------------------------------------------------------------

export function Card({ title, id, children, actions, sub }: { title: ReactNode; id?: string; children: ReactNode; actions?: ReactNode; sub?: ReactNode }) {
  return (
    <section className="card" data-testid={id}>
      <div className="card-head">
        <div><h2>{title}</h2>{sub && <p className="muted sub">{sub}</p>}</div>
        {actions && <div className="actions">{actions}</div>}
      </div>
      {children}
    </section>
  );
}

/** A control with its visible label above it, and optional help below. */
export function Field({ label, children, grow, help }: { label: string; children: ReactNode; grow?: boolean; help?: ReactNode }) {
  return (
    <label className={grow ? 'field grow' : 'field'}>
      <span className="label">{label}</span>
      {children}
      {help && <small className="help">{help}</small>}
    </label>
  );
}

/** False, with the browser's message on the picker, when a required picker of the form has nothing chosen. */
function pickersFilled(form: HTMLFormElement): boolean {
  const empty = [...form.querySelectorAll<HTMLInputElement>('input[data-required]')].find((i) => !i.value);
  if (!empty) return true;
  const visible = empty.parentElement?.querySelector<HTMLInputElement>('input[role=combobox]');
  visible?.setCustomValidity(`Choose ${empty.dataset.required}.`);
  visible?.reportValidity();
  return false;
}

/** A form whose submit receives its fields as strings. With clearOnSuccess, a true result clears it. */
export function Form({ onSubmit, children, id, clearOnSuccess, className = 'inline' }: { onSubmit: (f: Record<string, string>) => unknown; children: ReactNode; id?: string; clearOnSuccess?: boolean; className?: string }) {
  const submit = async (e: FormEvent<HTMLFormElement>) => {
    e.preventDefault();
    const form = e.currentTarget;
    if (!pickersFilled(form)) return;
    const f = Object.fromEntries([...new FormData(form)].map(([k, v]) => [k, String(v)]));
    if ((await onSubmit(f)) === true && clearOnSuccess) form.reset();
  };
  return <form className={className} onSubmit={submit} data-testid={id}>{children}</form>;
}

// ---- records ------------------------------------------------------------------------------------------------------

/**
 * A table of records with a toolbar: the filter, the count and the add action (Records First Views). `text` gives
 * the searchable text of a record; `row` renders it as a <tr>.
 */
export function Records<T>({ title, id, items, text, head, row, add, empty, tools, sub, error, filter = true }: {
  title: string; id?: string; items: T[] | null | undefined; text: (t: T) => string; head: ReactNode; row: (t: T) => ReactNode;
  add?: ReactNode; empty: ReactNode; tools?: ReactNode; sub?: ReactNode; error?: string; filter?: boolean;
}) {
  const [q, setQ] = useState('');
  const all = items ?? [];
  const shown = q ? all.filter((t) => text(t).toLowerCase().includes(q.toLowerCase())) : all;
  const cols = 12;
  return (
    <section className="card records" data-testid={id}>
      <div className="card-head">
        <div>
          <h2>{title} {items && <span className="tally" data-testid={id && `${id}-count`}>{q ? `${shown.length} of ${all.length}` : all.length}</span>}</h2>
          {sub && <p className="muted sub">{sub}</p>}
        </div>
        <div className="actions">
          {filter && all.length > 5 && (
            <input className="filter" type="search" placeholder="Filter" aria-label={`Filter ${title.toLowerCase()}`} value={q} onChange={(e) => setQ(e.target.value)} data-testid={id && `${id}-filter`} />
          )}
          {tools}
          {add}
        </div>
      </div>
      {error && <p className="err" role="alert">{error}</p>}
      <div className="scroll">
        <table>
          <thead>{head}</thead>
          <tbody>
            {shown.map(row)}
            {items && all.length === 0 && <tr><td colSpan={cols} className="empty">{empty}</td></tr>}
            {items && all.length > 0 && shown.length === 0 && <tr><td colSpan={cols} className="empty">Nothing matches “{q}”.</td></tr>}
            {!items && !error && <tr><td colSpan={cols} className="empty">Loading…</td></tr>}
          </tbody>
        </table>
      </div>
    </section>
  );
}

/** Values as small chips. */
export function Chips({ values, empty = '—' }: { values: readonly string[]; empty?: string }) {
  if (!values.length) return <span className="muted">{empty}</span>;
  return <span className="chips">{values.map((v) => <span key={v} className="chip">{v}</span>)}</span>;
}

// ---- dialogs ------------------------------------------------------------------------------------------------------

/**
 * A modal dialog with a form (Record Dialogs). `onSubmit` returns the success note, or throws; on success the dialog
 * closes and shows the note, on failure it stays open with the error and the entered values.
 */
export function Dialog({ title, id, onClose, onSubmit, submit = 'Save', danger, wide, children, noteId, intro }: {
  title: string; id?: string; onClose: () => void; onSubmit?: (f: Record<string, string>) => Promise<string | void>;
  submit?: string; danger?: boolean; wide?: boolean; children: ReactNode; noteId?: string; intro?: ReactNode;
}) {
  const ref = useRef<HTMLDialogElement>(null);
  const [error, setError] = useState<Error | null>(null);
  const [busy, setBusy] = useState(false);
  const titleId = useId();
  useEffect(() => {
    const d = ref.current!;
    if (!d.open) d.showModal();
    d.querySelector<HTMLElement>('input:not([type=hidden]), select, textarea, button.primary')?.focus();
    return () => d.close();
  }, []);
  const send = async (e: FormEvent<HTMLFormElement>) => {
    e.preventDefault();
    if (!onSubmit) return onClose();
    if (!pickersFilled(e.currentTarget)) return;
    const f =Object.fromEntries([...new FormData(e.currentTarget)].map(([k, v]) => [k, String(v)]));
    setBusy(true);
    try {
      const note = await onSubmit(f);
      setError(null);
      onClose();
      if (note) notify(note, noteId);
    } catch (err) {
      setError(err as Error);
    } finally {
      setBusy(false);
    }
  };
  return (
    <dialog ref={ref} className={wide ? 'modal wide' : 'modal'} aria-labelledby={titleId} onCancel={(e) => (e.preventDefault(), onClose())} data-testid={id && `${id}-dialog`}>
      <form method="dialog" onSubmit={send} data-testid={id} noValidate={false}>
        <header>
          <h2 id={titleId}>{title}</h2>
          <button type="button" className="icon" aria-label="Close" onClick={onClose}>×</button>
        </header>
        <div className="body">
          {intro && <div className="intro">{intro}</div>}
          {children}
        </div>
        <footer>
          {error && <p className="err" role="alert" data-testid={id && `${id}-error`}>{error.message}</p>}
          <button type="button" onClick={onClose} data-testid={id && `${id}-cancel`}>{onSubmit ? 'Cancel' : 'Close'}</button>
          {onSubmit && <button className={danger ? 'danger solid' : 'primary'} disabled={busy} data-testid={id && `${id}-submit`}>{busy ? 'Working…' : submit}</button>}
        </footer>
      </form>
    </dialog>
  );
}

/** Which dialog of a view is open, with its subject. */
export function useDialog<T = true>(): [T | null, (t: T) => void, () => void] {
  const [open, setOpen] = useState<T | null>(null);
  return [open, (t) => setOpen(() => t), () => setOpen(null)];
}

// ---- confirmation dialog (Confirmation Dialogs) ---------------------------------------------------------------------

interface Ask { title: string; body: ReactNode; action: string; danger?: boolean; reason?: 'required' | 'optional'; reasonLabel?: string; resolve: (r: string | null) => void }
let asking: Ask | null = null;
const askListeners = new Set<() => void>();

/** Asks before an action; resolves to the reason ('' when none was asked), or null when cancelled. */
export function ask(o: Omit<Ask, 'resolve'>): Promise<string | null> {
  return new Promise((resolve) => {
    asking?.resolve(null);
    asking = { danger: true, ...o, resolve };
    askListeners.forEach((l) => l());
  });
}

export function Asker() {
  const a = useStore(() => asking, askListeners);
  if (!a) return null;
  const done = (r: string | null) => {
    asking = null;
    askListeners.forEach((l) => l());
    a.resolve(r);
  };
  return (
    <Dialog key={a.title + a.action} id="confirm" title={a.title} submit={a.action} danger={a.danger} onClose={() => done(null)} onSubmit={async (f) => void done(f.reason ?? '')}>
      <div data-testid="confirm-text">{a.body}</div>
      {a.reason && (
        <Field label={a.reasonLabel ?? (a.reason === 'required' ? 'Reason' : 'Reason (optional)')} grow>
          <input name="reason" data-testid="confirm-reason" required={a.reason === 'required'} autoComplete="off" />
        </Field>
      )}
    </Dialog>
  );
}

// ---- pickers (Pickers For Known Values) -----------------------------------------------------------------------------

export interface Option { value: string; label?: string; hint?: string }

/**
 * A searchable list (ARIA combobox). The chosen values are submitted in a hidden input named `name`, comma-separated
 * when `multiple`. With `allowNew`, typed text that is not an option is accepted and marked as new.
 */
export function Picker({ name, options, multiple, allowNew, defaultValue = [], placeholder, testid, onChange, newHint = 'new', required, label }: {
  name: string; options: Option[]; multiple?: boolean; allowNew?: boolean; defaultValue?: string[] | string; placeholder?: string;
  testid?: string; onChange?: (v: string[]) => void; newHint?: string; required?: boolean; label?: string;
}) {
  const initial = Array.isArray(defaultValue) ? defaultValue : defaultValue ? [defaultValue] : [];
  const [chosen, setChosen] = useState<string[]>(initial);
  const [q, setQ] = useState(multiple ? '' : (initial[0] ?? ''));
  const [open, setOpen] = useState(false);
  const [hi, setHi] = useState(0);
  const listId = useId();
  const known = useMemo(() => new Map(options.map((o) => [o.value.toLowerCase(), o])), [options]);
  const query = q.trim().toLowerCase();
  const matches = options
    .filter((o) => !(multiple && chosen.some((c) => c.toLowerCase() === o.value.toLowerCase())))
    .filter((o) => !query || `${o.value} ${o.label ?? ''} ${o.hint ?? ''}`.toLowerCase().includes(query))
    .slice(0, 50);
  const offerNew = allowNew && query && !known.has(query) && !chosen.some((c) => c.toLowerCase() === query);
  const items: (Option & { isNew?: boolean })[] = [...matches, ...(offerNew ? [{ value: q.trim(), isNew: true }] : [])];

  const set = (v: string[]) => (setChosen(v), onChange?.(v));
  const pick = (v: string) => {
    const canonical = known.get(v.toLowerCase())?.value ?? v;
    if (multiple) {
      set([...chosen, canonical]);
      setQ('');
    } else {
      set([canonical]);
      setQ(canonical);
      setOpen(false);
    }
    setHi(0);
  };
  const commitTyped = () => {
    const t = q.trim();
    if (!t) return multiple ? undefined : set([]);
    if (known.has(t.toLowerCase()) || allowNew) {
      if (multiple ? !chosen.some((c) => c.toLowerCase() === t.toLowerCase()) : chosen[0] !== t) pick(t);
    } else if (!multiple) {
      setQ(chosen[0] ?? '');
    }
  };
  const key = (e: KeyboardEvent<HTMLInputElement>) => {
    if (e.key === 'ArrowDown') (e.preventDefault(), setOpen(true), setHi((h) => Math.min(h + 1, items.length - 1)));
    else if (e.key === 'ArrowUp') (e.preventDefault(), setHi((h) => Math.max(h - 1, 0)));
    else if (e.key === 'Enter' && open && items[hi]) (e.preventDefault(), pick(items[hi]!.value));
    else if (e.key === 'Escape' && open) (e.preventDefault(), e.stopPropagation(), setOpen(false));
    else if (e.key === 'Backspace' && multiple && !q && chosen.length) set(chosen.slice(0, -1));
  };
  const isNew = (v: string) => !known.has(v.toLowerCase());
  // Typed text counts as chosen when it is allowed, so submitting without leaving the field keeps it.
  const typed = q.trim();
  const typedValue = typed && (allowNew || known.has(typed.toLowerCase())) ? (known.get(typed.toLowerCase())?.value ?? typed) : '';
  const value = multiple
    ? [...chosen, ...(typedValue && !chosen.some((c) => c.toLowerCase() === typedValue.toLowerCase()) ? [typedValue] : [])].join(',')
    : typedValue;
  return (
    <div className={`picker${multiple ? ' multi' : ''}`} data-testid={testid}>
      {multiple && chosen.map((c) => (
        <span key={c} className={`chip${isNew(c) ? ' new' : ''}`} title={isNew(c) ? newHint : undefined}>
          {c}{isNew(c) && <small> ({newHint})</small>}
          <button type="button" aria-label={`Remove ${c}`} onClick={() => set(chosen.filter((x) => x !== c))}>×</button>
        </span>
      ))}
      <input
        role="combobox"
        aria-expanded={open}
        aria-controls={listId}
        aria-autocomplete="list"
        aria-label={label}
        aria-activedescendant={open && items[hi] ? `${listId}-${hi}` : undefined}
        value={q}
        placeholder={multiple && chosen.length ? '' : placeholder}
        aria-required={required || undefined}
        autoComplete="off"
        onChange={(e) => (e.target.setCustomValidity(''), setQ(e.target.value), setOpen(true), setHi(0), !multiple && e.target.value === '' && set([]))}
        onFocus={() => setOpen(true)}
        onBlur={() => setTimeout(() => (setOpen(false), commitTyped()), 120)}
        onKeyDown={key}
        data-testid={testid && `${testid}-input`}
      />
      {!multiple && q && chosen[0] === q && isNew(q) && allowNew && <small className="new-mark">{newHint}</small>}
      <input type="hidden" name={name} value={value} readOnly data-required={required ? (label ?? name) : undefined} />
      {open && items.length > 0 && (
        <ul id={listId} role="listbox" className="options">
          {items.map((o, i) => (
            <li
              key={`${o.value}-${i}`}
              id={`${listId}-${i}`}
              role="option"
              aria-selected={i === hi}
              className={i === hi ? 'hi' : ''}
              onMouseDown={(e) => (e.preventDefault(), pick(o.value))}
              onMouseEnter={() => setHi(i)}
            >
              {o.isNew ? <>Use “{o.value}” <small>({newHint})</small></> : <>{o.label ?? o.value}{o.hint && <small> {o.hint}</small>}</>}
            </li>
          ))}
        </ul>
      )}
    </div>
  );
}

// ---- known values -------------------------------------------------------------------------------------------------

interface Directory { users: { name: string; sources: string[]; bootstrap: boolean }[]; idp_roles: string[] }
const cache = new Map<string, Promise<unknown>>();
function cached<T>(path: string): Promise<T> {
  if (!cache.has(path)) cache.set(path, api<T>('GET', path).catch((e) => (cache.delete(path), Promise.reject(e))));
  return cache.get(path) as Promise<T>;
}
/** Forgets the known users and Groups after a change that adds some. */
export const refreshKnown = () => cache.clear();

function useCached<T>(path: string | null, fallback: T): T {
  const [v, setV] = useState<T>(fallback);
  useEffect(() => {
    if (!path) return;
    let live = true;
    cached<T>(path).then((d) => live && setV(d), () => {});
    return () => void (live = false);
  }, [path]);
  return v;
}

const SOURCE: Record<string, string> = { installation: 'signed in', member: 'Group member', administrator: 'administrator', owner: 'Group owner', denied: 'denied' };

/** Known users as picker options (Admin Directory). */
export function useUsers(): Option[] {
  const d = useCached<Directory>('/api/v1/admin/identity/directory', { users: [], idp_roles: [] });
  return useMemo(() => d.users.map((u) => ({ value: u.name, hint: u.bootstrap ? 'bootstrap administrator' : u.sources.map((s) => SOURCE[s] ?? s).join(', ') })), [d]);
}
export function useIdpRoles(): Option[] {
  const d = useCached<Directory>('/api/v1/admin/identity/directory', { users: [], idp_roles: [] });
  return useMemo(() => d.idp_roles.map((r) => ({ value: r })), [d]);
}
export function useBootstrap(): string[] {
  const d = useCached<Directory>('/api/v1/admin/identity/directory', { users: [], idp_roles: [] });
  return useMemo(() => d.users.filter((u) => u.bootstrap).map((u) => u.name), [d]);
}
/** Known Groups as picker options (Every Known Group Is Listed); empty for roles without the policy view. */
export function useGroups(enabled = true): Option[] {
  const g = useCached<{ name: string; description: string; defined: boolean }[]>(enabled ? '/api/v1/admin/policy/groups' : null, []);
  return useMemo(() => g.map((x) => ({ value: x.name, hint: x.description || (x.defined ? '' : 'not defined') })), [g]);
}

export const ADMIN_ROLES: { role: string; what: string }[] = [
  { role: 'platform-admin', what: 'Manages everything except individual-level data.' },
  { role: 'security-auditor', what: 'Reads the audit log and, with a reason, individual-level data.' },
  { role: 'group-owner', what: 'Edits the policy of the Groups they own.' },
  { role: 'skill-reviewer', what: 'Reviews and revokes Skills shared with the organization.' },
  { role: 'works-council-reviewer', what: 'Reviews who accessed individual-level data.' },
];

// ---- tabs (View Sections) -------------------------------------------------------------------------------------------

/** The tab of a view, kept in the address as #/<view>/<tab>. */
export function useTab(view: string, tabs: readonly string[]): [string, (t: string) => void] {
  const read = () => {
    const t = location.hash.replace(/^#\/?/, '').split('/')[1] ?? '';
    return tabs.includes(t) ? t : tabs[0]!;
  };
  const [tab, setTab] = useState(read);
  useEffect(() => {
    const on = () => setTab(read());
    addEventListener('hashchange', on);
    return () => removeEventListener('hashchange', on);
  });
  return [tab, (t) => (history.replaceState(null, '', `#/${view}/${t}`), setTab(t))];
}

export function Tabs({ tabs, value, onChange, counts }: { tabs: { id: string; label: string }[]; value: string; onChange: (t: string) => void; counts?: Record<string, number | undefined> }) {
  return (
    <div className="view-tabs" role="tablist">
      {tabs.map((t) => (
        <button key={t.id} type="button" role="tab" aria-selected={value === t.id} className={value === t.id ? 'on' : ''} data-testid={`tab-${t.id}`} onClick={() => onChange(t.id)}>
          {t.label}{counts?.[t.id] !== undefined && <span className="tally">{counts[t.id]}</span>}
        </button>
      ))}
    </div>
  );
}

export const time = (s: string | null | undefined) => (s ? new Date(s).toLocaleString() : '');
export const show = (v: unknown) => (typeof v === 'string' ? v : JSON.stringify(v));
export const qs = (o: Record<string, string | number | boolean | undefined | null>) =>
  new URLSearchParams(Object.entries(o).filter(([, v]) => v !== undefined && v !== null && v !== '').map(([k, v]) => [k, String(v)])).toString();
export const list = (s: string | undefined) => (s ?? '').split(',').map((x) => x.trim()).filter(Boolean);
