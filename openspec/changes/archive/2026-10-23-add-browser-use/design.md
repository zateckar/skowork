# Design

## Context

- **Chrome 136+:** Chrome ignores `--remote-debugging-port` and `--remote-debugging-pipe` on the default user-data directory. A dedicated `--user-data-dir` is therefore mandatory.
- **Enterprise policy:** the organization Chrome policy allows remote debugging (user decision, 2026-10-21). The installed Chrome version and the policy are owned by the desktop team. A future policy change could still set `RemoteDebuggingAllowed=false`, `DeveloperToolsAvailability` or `BrowserSignin`, so the startup check in D6 stays as a defensive measure.
- **Tool plumbing:** tools, decisions and the quarantine folder come from earlier changes; see proposal Impact.

## Goals / Non-Goals

**Goals:**
- Reliable, low-token web automation, using the accessibility snapshot first and vision second.
- Zero exposure of the user's own browser profile, and zero password handling by the agent.

**Non-Goals:**
- A Chrome extension bridge to the user's real tabs. This could be a later change using `ExtensionInstallForcelist` and `chrome.debugger`.
- Bundling Chromium. We use the organization-managed Chrome.

## Decisions

### D1. CDP over `--remote-debugging-pipe`
- **Choice:** the client launches `chrome.exe --user-data-dir=<data root>\browser-profile --remote-debugging-pipe --no-first-run` and passes inherited pipe handles (fd 3/4 semantics on Windows through `STARTUPINFO` handle inheritance). The protocol is NUL-delimited JSON over the pipe.
- **Why:** with a pipe, no other process can attach, and no port is listening.
- **Alternatives considered:** a debugging port on localhost, which any local process could reach; Playwright or Puppeteer, which need a Node runtime (breaks the footprint).

### D2. Accessibility snapshot with refs
- **Choice:** use `Accessibility.getFullAXTree` together with `DOM.describeNode`. Each interactive node gets a short ref (`e12`) that maps to its `backendNodeId`. Snapshots are pruned to interactive and landmark nodes, with text truncated per node. Actions resolve the ref, scroll it into view and dispatch `Input.dispatchMouseEvent` / `Input.insertText`.
- **Alternatives considered:**
  - Raw DOM HTML: too many tokens.
  - Vision-only: grounding on the on-prem model is unproven, and it is slower.

### D3. Password and credential detection
- **Choice:** before a `type` action, the client refuses it if the target element:
  - is `input[type=password]`;
  - has `autocomplete` set to `current-password`, `new-password` or `one-time-code`;
  - is inside a known identity-provider sign-in page.
  The client then raises the hand-over Question through client/agent-runtime.
- **Alternative considered:** an approval prompt for password typing. Rejected, because the agent must never see or hold credentials.

### D4. Submit detection
- **Choice:** a click is classified as external if the target is a submit button or input, a button inside a form that has an action, or an element whose role and name match confirm/submit/send/order/pay in cs, de or en. Navigation to a new registrable domain is also external.
- **Alternative considered:** treating every click as external. Rejected because of approval fatigue.

### D5. Egress enforcement through CDP request interception
- **Choice:** enable `Fetch.enable` with a catch-all request pattern. Every paused request, including navigations, redirects and subresources, goes through the egress check of client/policy-enforcement. The request is either continued or failed with `BlockedByClient`. Chrome's own proxy is also pointed at the client egress proxy (`--proxy-server`), so traffic that CDP interception does not see, such as service workers, is still constrained.
- **Alternatives considered:**
  - Top-level navigation checks only: subresources could exfiltrate data.
  - The Chrome `URLBlocklist` policy: it is machine-wide and not per user Group.

### D6. Policy check at startup
- **Choice:** read the Chrome policy registry keys (`HKLM`/`HKCU\SOFTWARE\Policies\Google\Chrome`) for `RemoteDebuggingAllowed`, then confirm with a CDP handshake. If the handshake fails within 10 seconds, the tool is withdrawn and the failure reason is surfaced.
- **Why keep it when debugging is allowed:** the current organization policy allows remote debugging, but the policy is owned outside this project and can change. A clear withdrawal is better than failing tool calls.

### D7. Screenshots reach the model as image tool results
- **Choice:** browser.screenshot returns its image in the tool result. The runtime attaches it to the next model request only, as a user message after the tool results, and sends that request with the `vision` role (client/agent-runtime Image Tool Results). The main and vision roles map to the same upstream model today (add-agent-runtime design D0), so tool calling continues in that turn. The image is prepared by the model client's downscaling (longest side 1568 px, 5 MB), never stored in the Thread, and dropped after the request. With policy.tools.browser.retain_screenshots the PNG is also written under `<data root>\screenshots\<run id>\`, encrypted with DPAPI for the current user.
- **Alternatives considered:**
  - The tool calls the vision role itself and returns a text description: the main model cannot look at the page and has to guess what to ask.
  - Images kept in the conversation for the rest of the Run: every later request would grow by an image and need the vision role.

### D8. Risk class from the page and the Run
- **Choice:** a tool may compute its Risk class from the Run as well as from the arguments. Browser tools use this to classify a call before it runs: the target element's facts (tag, type, role, accessible name, enclosing form with an action) come from the last snapshot of the Run's tab, and the Run's starting site is the registrable domain of the first page the Run navigated to or found open. Registrable domains use the last two labels, or three when the second-level label is a common public suffix (co, com, ac, gov, org, net with a two-letter country code). Submit words: cs odeslat, potvrdit, objednat, zaplatit, uložit, smazat; de senden, absenden, bestätigen, bestellen, bezahlen, speichern, löschen; en submit, send, confirm, order, pay, save, delete, buy.
- **Alternatives considered:** classifying after the call: the decision must be made before the click happens.

### D9. Chrome's proxy and the egress proxy credential
- **Choice:** Chrome starts with `--proxy-server` pointing at the egress proxy of client/tool-sandbox, under a proxy session owned by the browser. Chrome cannot carry the credential in the proxy URL, so the client enables `Fetch.enable` with `handleAuthRequests` and answers the proxy's challenge (`source: Proxy`) with the session credential; server challenges are cancelled, so the agent never answers a site's own sign-in. Loopback hosts bypass the proxy, as Chrome does by default, and are still checked by the `Fetch` interception.
- **Alternative considered:** a proxy port without a credential for the browser: any local process could use it to reach allowed hosts.

### D10. Withdrawing tools
- **Choice:** a tool definition can say whether it is offered right now. Browser tools are offered while the Chrome check passes (D6); browser.eval also needs policy.tools.browser.allow_eval. A withdrawn tool is neither listed, found by find_tools, nor callable. The Connections section of the settings shows the browser state and the failure reason (`browser.status`).
- **Minimum Chrome version:** 136, the version from which the dedicated `--user-data-dir` is required for remote debugging. Task 1.1 can raise it.

### D11. Lifecycle, tabs and downloads
- **Choice:**
  - One agent browser per client, shared by Runs. Each Run works in its own tab, opened on its first browser call and closed when the Run ends; `browser.tabs` lists and switches only among the Run's tabs.
  - The browser closes after the idle timeout, unless the user is interacting with it: its window is in the foreground and the last user input is less than 2 minutes old.
  - Downloads go to `<data root>\browser-downloads` (`Browser.setDownloadBehavior` with `allowAndName`); a completed download is moved into the quarantine with `quarantine_save` and reported in the result of the action that started it (waiting up to 10 s).
  - The visible window is brought to the front for the sign-in hand-over with `Page.bringToFront` and the window's foreground permission.
  - Chrome asks for a client certificate in a native dialog that CDP cannot see or answer, and `Page.navigate` does not return until it is answered. Found 2026-10-23 on the organization identity provider, which requests an optional client certificate, on a workstation with several organization certificates and no `AutoSelectCertificateForUrls` policy. A navigation that has not committed after 30 s is therefore handed over to the user in a visible window ("the page is waiting for you, for example for a certificate choice"); headless, it fails with that reason. An `AutoSelectCertificateForUrls` entry for the identity provider in the organization Chrome policy would remove the prompt.
- **Alternatives considered:** one browser per Run, which loses the session between Runs and costs a Chrome start (about 1 s) per Run.

## Risks / Trade-offs

- [A future Chrome policy change disallows remote debugging] → Remote debugging is allowed today (confirmed 2026-10-21). Task 1.1 confirms this on a managed laptop. If the policy changes later, the D6 check withdraws browser tools, and the fallback is a later extension-bridge change.
- [Prompt injection from web pages] → Snapshots are marked untrusted with provenance. Submit and cross-site actions are external, and domain policy applies.
- [The dedicated profile diverges from the user's expectations, for example bookmarks and sign-ins] → The UI explains that it is the agent's own browser. Sign-in hand-over works inside it.
- [Screenshots contain sensitive data] → By default they are held in memory only for the duration of the Run. They are written to the data root, encrypted, only when policy.tools.browser.retain_screenshots is true.

## Validation Results (2026-10-23)

- **Task 1.1, on this managed workstation:** Chrome 155.0.8059.80. The machine Chrome policy allows remote debugging. The dedicated profile starts with `--remote-debugging-pipe`, the CDP handshake completes, and no Chrome process listens on TCP. Organization sign-in is OAuth through Keycloak, not Kerberos, so the browser no longer sets `--auth-server-allowlist`. Opening the realm's account console leads to the identity provider. Chrome then waits for a client certificate choice (D11), and that wait is handed over to the user with the sign-in Question.
- **Task 1.2, the real main model on public sites** (`client/tests/e2e/browser_e2e_test.cpp`; Chrome in a window through the egress proxy; write-local allowed, external approved):

  | Site | Task | Outcome | Snapshots (median / max) |
  |------|------|---------|--------------------------|
  | Wikipedia | read the founding year from the Acme Auto infobox, then search for "Acme Falcon" and read the first sentence | succeeded; the submit raised 1 Approval showing the page as provenance | ~6.0k tokens / 24k chars (the snapshot limit) |
  | Amazon | search "usb-c cable", report the first result's name and price, buy nothing | succeeded, with 3 Approvals (country notice, search) | ~2.5k tokens / 24k chars |
  | Google | reject cookies, search, report the first result | the consent rejection needed an Approval; Google then showed its "unusual traffic" check, which the agent handed over to the user (`browser.wait` with `user`) | ~0.45k tokens / 2k chars |

  Long articles reach the 24,000-character snapshot limit, which is about 6k tokens. The agent still found what it needed in them, so the limit stays as it is.

## Open Questions

- What is the exact minimum Chrome version that is supported? It will be set from the task 1.1 results and does not change the specs.
- Resolved (2026-10-21): the organization Chrome policy allows remote debugging.
