// A minimal headless-browser driver for the admin console end-to-end tests (add-telemetry-and-admin design D10):
// an installed Chrome or Edge with a temporary profile, driven over the DevTools protocol with the `ws` client.
import { spawn, type ChildProcess } from 'node:child_process';
import { existsSync, mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import WebSocket from 'ws';
import { sleep } from './helpers.ts';

/** The browser to use: WORKBENCH_E2E_BROWSER, else an installed Chrome or Edge; null when there is none. */
export function findBrowser(): string | null {
  const env = process.env.WORKBENCH_E2E_BROWSER;
  if (env) return existsSync(env) ? env : null;
  const pf = process.env.ProgramFiles ?? 'C:\\Program Files';
  const pf86 = process.env['ProgramFiles(x86)'] ?? 'C:\\Program Files (x86)';
  const local = process.env.LOCALAPPDATA ?? '';
  const candidates =
    process.platform === 'win32'
      ? [
          join(pf, 'Google\\Chrome\\Application\\chrome.exe'),
          join(pf86, 'Google\\Chrome\\Application\\chrome.exe'),
          join(local, 'Google\\Chrome\\Application\\chrome.exe'),
          join(pf86, 'Microsoft\\Edge\\Application\\msedge.exe'),
          join(pf, 'Microsoft\\Edge\\Application\\msedge.exe'),
        ]
      : process.platform === 'darwin'
        ? ['/Applications/Google Chrome.app/Contents/MacOS/Google Chrome', '/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge']
        : ['/usr/bin/google-chrome', '/usr/bin/google-chrome-stable', '/usr/bin/chromium', '/usr/bin/chromium-browser', '/usr/bin/microsoft-edge'];
  return candidates.find((c) => c && existsSync(c)) ?? null;
}

export class Browser {
  readonly #proc: ChildProcess;
  readonly #profile: string;
  readonly #ws: WebSocket;
  #id = 0;
  readonly #pending = new Map<number, { resolve: (v: any) => void; reject: (e: Error) => void }>();
  readonly errors: string[] = [];
  readonly #loads: (() => void)[] = [];

  private constructor(proc: ChildProcess, profile: string, ws: WebSocket) {
    this.#proc = proc;
    this.#profile = profile;
    this.#ws = ws;
    ws.on('message', (d) => {
      const m = JSON.parse(d.toString());
      if (m.id !== undefined) {
        const p = this.#pending.get(m.id);
        this.#pending.delete(m.id);
        if (m.error) p?.reject(new Error(`${m.error.message}`));
        else p?.resolve(m.result);
      } else if (m.method === 'Runtime.exceptionThrown') {
        this.errors.push(m.params.exceptionDetails?.exception?.description ?? m.params.exceptionDetails?.text);
      } else if (m.method === 'Page.loadEventFired') {
        for (const w of this.#loads.splice(0)) w();
      }
    });
  }

  static async launch(exe: string): Promise<Browser> {
    const profile = mkdtempSync(join(tmpdir(), 'workbench-e2e-'));
    const proc = spawn(exe, [
      '--headless=new', '--remote-debugging-port=0', `--user-data-dir=${profile}`, '--no-first-run', '--no-default-browser-check',
      '--disable-extensions', '--disable-sync', '--disable-background-networking', '--disable-component-update', '--window-size=1400,1000',
      // CI runners on Linux restrict the user namespaces Chrome's sandbox needs; the page is our own app on loopback.
      ...(process.platform === 'linux' && process.env.CI ? ['--no-sandbox'] : []),
      'about:blank',
    ], { stdio: 'ignore' });
    const portFile = join(profile, 'DevToolsActivePort');
    const deadline = Date.now() + 20_000;
    while (!existsSync(portFile) || readFileSync(portFile, 'utf8').split('\n').length < 2) {
      if (Date.now() > deadline || proc.exitCode !== null) throw new Error('the browser did not start');
      await sleep(100);
    }
    const port = readFileSync(portFile, 'utf8').split('\n')[0]!.trim();
    let target: { type: string; webSocketDebuggerUrl: string } | undefined;
    while (!target) {
      const list = (await (await fetch(`http://127.0.0.1:${port}/json/list`)).json()) as { type: string; webSocketDebuggerUrl: string }[];
      target = list.find((t) => t.type === 'page');
      if (!target) await sleep(100);
    }
    const ws = new WebSocket(target.webSocketDebuggerUrl, { perMessageDeflate: false });
    await new Promise<void>((resolve, reject) => (ws.once('open', () => resolve()), ws.once('error', reject)));
    const b = new Browser(proc, profile, ws);
    await b.send('Page.enable');
    await b.send('Runtime.enable');
    // Confirmation dialogs are accepted.
    await b.send('Page.addScriptToEvaluateOnNewDocument', { source: 'window.confirm = () => true;' });
    return b;
  }

  send(method: string, params: Record<string, unknown> = {}): Promise<any> {
    const id = ++this.#id;
    return new Promise((resolve, reject) => {
      this.#pending.set(id, { resolve, reject });
      this.#ws.send(JSON.stringify({ id, method, params }));
    });
  }

  async eval<T = unknown>(expression: string): Promise<T> {
    const r = await this.send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true });
    if (r.exceptionDetails) throw new Error(`page: ${r.exceptionDetails.exception?.description ?? r.exceptionDetails.text}`);
    return r.result.value as T;
  }

  async goto(url: string): Promise<void> {
    const loaded = new Promise<void>((r) => this.#loads.push(r));
    const nav = await this.send('Page.navigate', { url });
    // A new document has a loader; a change of the fragment only is a same-document navigation without a load event.
    if (nav.loaderId) await Promise.race([loaded, sleep(15_000).then(() => Promise.reject(new Error(`no load event for ${url}`)))]);
  }

  /** Waits until `expression` is truthy in the page; returns its value. */
  async waitFor<T = unknown>(expression: string, ms = 8000): Promise<T> {
    const deadline = Date.now() + ms;
    let last: unknown;
    for (;;) {
      try {
        last = await this.eval(expression);
        if (last) return last as T;
      } catch (e) {
        last = e;
      }
      if (Date.now() > deadline) {
        const body = await this.eval<string>('document.body?.innerText ?? ""').catch(() => '');
        throw new Error(`timed out waiting for ${expression} (last ${String(last)}); page text:\n${body.slice(0, 2000)}`);
      }
      await sleep(50);
    }
  }

  /** Waits for the element with data-testid `id` and returns its text. */
  text(id: string, ms?: number): Promise<string> {
    return this.waitFor<string>(`document.querySelector('[data-testid="${id}"]')?.innerText`, ms);
  }

  has(id: string): Promise<boolean> {
    return this.eval<boolean>(`!!document.querySelector('[data-testid="${id}"]')`);
  }

  /** Clicks the element `selector` (a data-testid, or a CSS selector starting with "css:"). */
  async click(selector: string): Promise<void> {
    const q = this.#query(selector);
    await this.waitFor(`!!${q}`);
    await this.eval(`${q}.click()`);
  }

  /** Clicks the button with the exact label inside the element with data-testid `scope`. */
  async clickButton(scope: string, label: string): Promise<void> {
    const q = `[...document.querySelectorAll('[data-testid="${scope}"] button')].find((b) => b.innerText.trim() === ${JSON.stringify(label)})`;
    await this.waitFor(`!!(${q})`);
    await this.eval(`(${q}).click()`);
  }

  /** Sets form fields of the form with data-testid `form` by name (as typing would, so React sees it), then submits it. */
  async submit(form: string, fields: Record<string, string | boolean> = {}): Promise<void> {
    await this.waitFor(`!!document.querySelector('[data-testid="${form}"]')`);
    await this.eval(`(() => {
      const f = document.querySelector('[data-testid="${form}"]');
      for (const [name, value] of Object.entries(${JSON.stringify(fields)})) {
        const el = f.querySelector('[name="' + name + '"]');
        if (!el) throw new Error('no field ' + name);
        if (el.type === 'checkbox') { if (el.checked !== value) el.click(); continue; }
        const proto = el.tagName === 'SELECT' ? HTMLSelectElement.prototype : el.tagName === 'TEXTAREA' ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype;
        Object.getOwnPropertyDescriptor(proto, 'value').set.call(el, value);
        el.dispatchEvent(new Event(el.tagName === 'SELECT' ? 'change' : 'input', { bubbles: true }));
      }
      f.requestSubmit();
    })()`);
  }

  /** Sets the value of a single control (data-testid) as typing would. */
  async fill(id: string, value: string): Promise<void> {
    await this.waitFor(`!!document.querySelector('[data-testid="${id}"]')`);
    await this.eval(`(() => {
      const el = document.querySelector('[data-testid="${id}"]');
      const proto = el.tagName === 'SELECT' ? HTMLSelectElement.prototype : el.tagName === 'TEXTAREA' ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype;
      Object.getOwnPropertyDescriptor(proto, 'value').set.call(el, ${JSON.stringify(value)});
      el.dispatchEvent(new Event(el.tagName === 'SELECT' ? 'change' : 'input', { bubbles: true }));
    })()`);
  }

  #query(selector: string) {
    return selector.startsWith('css:') ? `document.querySelector(${JSON.stringify(selector.slice(4))})` : `document.querySelector('[data-testid="${selector}"]')`;
  }

  async close(): Promise<void> {
    await this.send('Browser.close').catch(() => {});
    this.#ws.terminate();
    await Promise.race([new Promise((r) => this.#proc.once('exit', r)), sleep(5000)]);
    if (this.#proc.exitCode === null) this.#proc.kill();
    for (let i = 0; i < 20; i++) {
      try {
        rmSync(this.#profile, { recursive: true, force: true });
        return;
      } catch {
        await sleep(250);
      }
    }
  }
}
