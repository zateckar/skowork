# Design

## Context

See proposal.md, section "Why".

What this design builds on:
- Skills run as subagents, as defined in client/agent-runtime. Tools are gated by client/policy-enforcement. Scripts run under client/tool-sandbox.
- The skill review UI is in control-plane/admin-console.
- Push delivery is handled by control-plane/fleet-registry.

The SKILL.md format is kept compatible with agentskills.io and Claude, so skills can be exchanged with the wider ecosystem after review.

## Goals / Non-Goals

**Goals:**
- Creating and sharing a skill with a colleague takes less than 5 minutes and needs no admin involvement.
- Org-wide skills are reviewed, signed and centrally revocable.

**Non-Goals:**
- Public distribution.
- Skill monetisation.
- Script runtimes beyond what policy provides.

## Decisions

### D1. The registry is a Node.js service backed by internal GitLab
- **Choice:** each skill is a Git repository in the internal GitLab group `workbench-skills`. Each published version is a signed tag. The registry is a module of the one control-plane process (Node.js with TypeScript and Express). It owns:
  - metadata, scopes and the review state machine, in SQLite (`node:sqlite`);
  - the scanner;
  - signing, with an ES256 key held in the secret store;
  - the distribution API.

  Clients never talk to GitLab. They use only the registry API and their Workbench token.
- **Why:** this gives us history, diffs, merge-request-style review for power users, and backups for free. Clients still have one simple, authenticated API.
- **Alternatives considered:**
  - Pure GitLab, where clients clone over Git: the client would need Git and GitLab credentials, and access control would be harder to align with Groups.
  - A dedicated blob-store registry without Git: we would lose diffs and history, and would need custom tooling.

### D2. Precedence and locking
- **Choice:** the tiers are built-in < org < group < shared < private, so a user's private copy wins by default. This supports personal adaptation.
- **Exception:** policy can lock org skills through `policy.skills.locked`, for compliance-critical procedures. Shadowing a locked skill is reported and ignored.
- **Alternative considered:** org always wins. Rejected because it blocks personal tweaks of generic org skills.

### D3. Tool scoping by intersection
- **Choice:** a skill's effective tools are its declared `tools` intersected with what the policy allows. A skill without `tools` gets the read-class tools that policy allows.
- **Stricter-only floor:** `interrupt_on` and `risk` feed the skill-declared decision floor of client/policy-enforcement. They can add approvals and restrict resolutions, but never remove an approval. `network: none` selects the no-network mode of client/tool-sandbox for the skill's scripts.
- **Why:** this keeps the rule simple: a skill can never escalate privileges.
- **Alternative considered:** per-skill grants by an admin. That is more flexible, but makes it harder to reason about what any given skill can do.

### D4. Scanner
- **Choice:** the scanner runs static checks:
  - secret detection, using a gitleaks ruleset;
  - extraction of PowerShell AST commands and URLs;
  - checks of declared tools and MCP servers;
  - a prompt-injection heuristic pattern set, applied to the body and to references.

  Results are advisory, except for secrets, which cause a hard reject. Review remains the gate.
- **Alternative considered:** LLM-based review only. It is non-deterministic and can itself be prompt-injected. It may be added later as an extra advisory signal.

### D5. Client sync
- **Choice:**
  - The client syncs its skill list at start, after reconnecting, on a skills-changed push, and at least every 24 hours. Every sync response also carries the revocations that apply to the user, so a missed skill-revoked push is caught up at the next sync.
  - Content is fetched by digest into `skills/<tier>/<name>/<version>/` under the data root.
  - The previous version is kept for rollback.
  - Signatures are verified against the registry's published key at load time.

### D6. Registry decisions that refine D1 to D5
- **Storage in SQLite.** The first version of the registry keeps Skill packages as rows in SQLite (`skills`, `skill_versions` with the zip bytes, `skill_usage_weekly`) instead of GitLab repositories (D1). Immutability, the review state machine and backups then live in one transactional store, and the registry has one dependency fewer. Clients see no difference, because they never talk to GitLab. A later change can mirror every published version into a GitLab repository for diffs and review by power users.
- **Package.** A zip with `SKILL.md` at the root or inside one top-level folder, and the optional folders `scripts/`, `references/` and `assets/` plus README and LICENSE files. The registry reads the frontmatter with a small YAML subset (scalars, inline and block lists, nested blocks preserved as text) and requires name, description and a semantic version, which the client treats as optional for private Skills only. The version is the top-level `version` or, as agentskills.io packages write it, `metadata.version`. The top-level field wins when both exist, and the registry and the client read it the same way, so a Skill has one version everywhere. (Added 2026-11-05: an agentskills.io package with `metadata.version` was refused.) Limits: 5 MB package, 200 entries, 20 MB unpacked (`SkillRegistry:MaxPackageBytes`).
- **Digest and signature.** The digest is the lower-case SHA-256 of the package bytes. Clients fetch content by digest. The signature of an org version is a compact JWS (ES256, `typ` `workbench-skill+jwt`) whose payload holds the name, version, digest, scope and issue time, so a client checks both the signature and that the digest is the one it downloaded. The key ring is its own (purpose `skills`) and rotated-out keys stay published.
- **Names and versions.** A Skill name belongs to the user who published it first. A version is unique per name and cannot change. Another scope or audience is a new version.
- **Scan.** All scopes are scanned. A secret finding rejects the version in every scope, because sharing a secret with named colleagues leaks it too. The secret rules are a built-in subset of well-known token formats and generic key assignments; findings never carry the secret. Everything else is advisory: script commands and URLs (a pattern-based extraction instead of a full PowerShell parser), risky commands (download and execute, encoded commands, recursive deletes, execution-policy changes), declared tools and MCP servers, injection phrases and hidden characters in the body and references, and personal data patterns (addresses, user profile paths).
- **Publishing rights.** The author's effective `policy.skills.publish` decides which scopes they may use (default shared). A platform-admin import skips that check but always goes to org review.
- **Review.** An org version is `in-review` until a skill-reviewer or platform-admin approves it. An author MUST NOT approve their own version, an import counts as its importer's. Approval signs and publishes; rejection needs a comment. Both notify the author through a skills-changed push.
- **Sole reviewer.** (Added 2026-11-05: a deployment with one platform-admin could not publish any org Skill it imported.) The self-review block yields when the author is the sole reviewer: nobody else is in `admin_roles` with platform-admin or skill-reviewer, and nobody else is in `WORKBENCH_BOOTSTRAP_ADMINS`. The approval then needs a justifying comment, sets `self_approved` on the version, and writes the audit event `skill.approve` with `selfApproved: true`. The block returns by itself as soon as a second reviewer is assigned. The review queue tells the sole reviewer that their approval will be recorded as a self-approval.
  - Alternatives considered: a configuration switch to allow self-review. It stays on after a second reviewer joins, and nobody notices. A break-glass role in Keycloak: it adds work for the operator without adding a second person.
  - [Risk] One person can sign and ship an org Skill. -> Mitigation: this is only possible where that person already controls the whole platform. The scan still runs and secrets are still rejected, and the self-approval stays visible on the version and in the audit log.
- **Distribution.** The list holds, for each name and scope, the highest published version the user may see (shared: named users; group: member Groups; org: everybody) with its digest and, for org, its signature. It also holds the revocations of the last 180 days (`SkillRegistry:RevocationRetentionDays`) that applied to the user's scopes, so that a client that missed a push removes the Skill at its next sync. Content is served by digest to users who may see a published version with that digest, and to its author.
- **Pushes.** skills-changed (`{name, version, scope, event}`) goes to the audience of the version: named users, Groups, or everyone. skill-revoked (`{name, versions: [{version, digest}], reason}`) goes to the audience of the revoked versions.
- **Revocation rights.** The author may revoke shared and group versions. A platform-admin or skill-reviewer may revoke any. Revoking a name without a version revokes every published version.
- **Usage.** Clients send counts per Skill name; the registry adds them to the current ISO week (Monday UTC) and stores neither a user nor an installation. Authors read the weeks of their own Skills.
- **API.** Clients use `/api/v1/skills`: `POST packages?scope=…&users=…|groups=…` with the zip as the body, `GET sync`, `GET content/{digest}`, `GET mine`, `POST {name}/versions/{version}/withdraw`, `POST {name}/revoke`, `POST usage` and `GET {name}/usage`. The public keys are at `/.well-known/workbench-skill-keys.json`. Reviewers use `/api/v1/admin/skills` (`queue`, `versions/{id}` with the scan, files and diff, `approve`, `reject`, `{name}/revoke`), and platform-admins also `import?source_url=…&licence=…` and `keys/rotate`. A public import takes one of two sources (revised 2026-11-05, because the form demanded both a URL and a file):
  - `?url=…`, with an empty body: the registry downloads the zip. HTTPS only; at most 5 redirects, each to HTTPS; 30 seconds; the response is cut at the package size limit. The URL is recorded as the source.
  - the zip as the body, with an optional `?source_url=…` that is recorded when given.
  Giving both `url` and a body is refused. `licence` is optional when SKILL.md has `license`. The download uses Node's `fetch`. Where the control plane reaches the internet only through a proxy, the operator sets `NODE_USE_ENV_PROXY=1` and `HTTPS_PROXY`, which Node 24 honours. Tests download over plain HTTP from a local server with `WORKBENCH_SKILLS_ALLOW_INSECURE_DOWNLOAD`, which is meant for development and tests only, like `WORKBENCH_MCP_ALLOW_INSECURE_HTTP`.
  - Alternatives considered: resolving agentskills.io or GitHub page URLs to archives. Every site has its own layout, and an archive URL works for all of them. Downloading in the admin console's browser: the browser's CORS rules block most hosts.
  - [Risk] A platform-admin's URL makes the control plane request an internal host (SSRF). -> Mitigation: only platform-admins can import, the request is a GET without credentials, and the response must be a valid Skill package; anything else is reported only as a validation error, never shown.
- **Diff.** The reviewer's diff compares the version with the highest lower version of the same name that was ever published, file by file, with a line diff for text files of up to 2000 lines.

### D7. Skills in the agent runtime
- **Selection.** The runtime has a reserved core tool `use_skill` (`skill`, `input`), offered to the root Run only and only while a Skill is usable. The names and descriptions of the usable Skills are appended to the root Run's system prompt when the Run starts executing, and stay fixed for that execution like the Run Context.
- **Subagent.** `use_skill` starts a subagent like `delegate`. Its `subagent_spec` fixes the tool set from the Skill Tool Scope over the registered tools that policy permits (runtime tools excluded), and carries the Skill's name, version, tier, folder, risk, network and interrupt_on rules. The Skill's body is the subagent's instructions. The `subagent` event names the Skill and lists the declared tools that were refused, so the Activity log shows the delegation; its entry is named "Skill <name>: <input>".
- **Floor and resolutions.** Each call of a Skill's subagent carries the Skill's interrupt_on patterns and risk to client/policy-enforcement as the skill-declared floor. An Approval it raises lists the accepted resolutions when the rules restrict them, and the runtime refuses any other resolution.
- **Explicit invocation.** When the newest user message of the root Run starts with `/name` and `name` is a usable Skill, the runtime records a `use_skill` call with the rest of the text as input instead of asking the model, so the Skill is used for that Turn. Any other `/word` is passed to the model as text.
- **Catalogue on disk.** Built-in Skills are folders next to the executable (`skills/<name>/`). Under the data root, private Skills are `skills/private/<name>/` and registry Skills `skills/<tier>/<name>/<version>/`, with a `pinned` file naming the version in use (otherwise the highest installed) and an `install.json` holding the provenance (the Group of a group Skill). A folder's name must equal the Skill's name, and a registry copy must declare the installed version. The Skills section of Settings shows each name with its active copy, tier, version, risk and tool scope, and every other copy as shadowed, conflicting or invalid with its error; a Skill whose required MCP servers are missing is unavailable and is offered neither to the agent nor to the composer.
- **Revocation.** Stopping a Skill cancels every subagent running it that has not ended, with the revocation as the reason. The parent's call gets "The Skill cancelled: <reason>" and the Run continues.

### D8. Client installation and lifecycle
- **When.** The client syncs when the control channel connects (start and every reconnect), on a skills-changed push, and when the last successful sync is 24 hours old. A skill-revoked push is applied at once.
- **Install.** For each listed version that `policy.skills.install_from` allows (org, group for all the user's Groups, `group:<name>` for one, shared), the client fetches the content by digest and checks its SHA-256. An org version also needs its registry signature: an ES256 JWS of type `workbench-skill+jwt`, verified with the published skill keys (cached, fetched again once for an unknown key), whose name, version, digest and scope are this version's. The client unpacks the zip itself with the registry's layout and limits (the digest ties the bytes to what the registry scanned), checks SKILL.md, and writes `<tier>/<name>/<version>/` with `package.zip` and `install.json` (digest, signature, Groups, author) through a temporary folder. A failed check records a security event and installs nothing.
- **Load-time check.** When the catalogue loads a registry copy, the package bytes must still match the digest, the files on disk must be exactly the package's, and an org copy's signature must verify; otherwise the copy is invalid and a security event is recorded.
- **Updates and rollback.** With `policy.skills.auto_update` (default true) a higher version is installed and the replaced one kept as the previous version; older ones are deleted. Without it, or while the user pinned a version, the higher version is only offered. Rollback uses the previous version and pins it until the user updates again; the version rolled back from stays offered.
- **Revocation.** A revoked version (push, or the revocations in the sync response) is deleted when its digest matches. When it was the version in use, the previous version is used, else the Skill is removed, and the subagents running the Skill are stopped with the reason.
- **Policy changes.** Installed Skills from a source `install_from` no longer allows are removed at the next sync.
- **Usage.** The client counts installs and the runs and failures of registry Skills (never private ones) in `usage.json` and reports them with the next sync.
- **Settings.** The Skills section shows a pin, offers the pending update and the rollback, and reloads the catalogue on request.

### D9. Skill scripts
- **Context.** Every tool call of a Skill's subagent carries the Skill's name, folder and network declaration in its tool context, so shell.run knows the call is a Skill's.
- **Runtimes.** For a Skill's call, shell.run finds the runtimes the command starts before anything runs: the programs in command position (the first word of each statement or pipeline element, after the `&` and `.` call operators, after an assignment, and the program of Start-Process), named by program (`python.exe`, `py`, `node`) or by script extension (`.py`, `.ps1`, `.js`, `.bat`). The names are powershell, python, node, cmd, bash, ruby, perl and wsh. The command itself is PowerShell, so powershell is always needed. When any needed runtime is missing from `policy.skills.script_runtimes`, the call fails with an error naming it and nothing starts. Calls outside a Skill are not checked. The check is lexical: a script that starts another runtime itself is not seen, and the Tool Sandbox still applies.
- **Network.** A Skill declaring `network: none` runs its shell.run processes in the no-network mode of client/tool-sandbox; otherwise they use the egress proxy under policy.network.
- **Suggested triggers.** They are offered through client/scheduler, so they were implemented after it (add-scheduled-and-remote-tasks). Each Skill version with a triggers field is offered once: when the catalogue first sees it usable (start, Skill sync, Save) or when it is first used. The offer is a standalone Question (capability `skill-triggers`) in a system-origin Thread that lists each suggestion and offers Create and Not now; `skill_trigger_offers` in `app.db` (migration 1300) records the version as offered, accepted or declined. Create passes each suggestion to the scheduler with the Skill attached, its name and a prompt filled in when missing, and condition fields written next to `kind` moved under `conditions`. The scheduler validates each one, so a suggestion it refuses (an unreadable folder, an unavailable kind) is logged and the others are still created. The Triggers are the user's (`created_by` user), because the user confirmed them. Draft test runs never offer.

### D10. Authoring, test runs and publishing
- **Editor.** The editor is a page of the window (`#/skills/new`, `#/skills/edit/<name>`), opened from the Skills section of Settings (New Skill; Edit on a private Skill). It has two text fields, the frontmatter (YAML) and the instructions (Markdown body), and checks them with the SKILL.md parser on every change; the first error names its field. Save is offered only while the Skill is valid. Saving writes `private/<name>/SKILL.md` through a temporary file, renames the folder when the name changed, refuses a name another private Skill already has, and reloads the catalogue, so new Runs can use the Skill at once. A private Skill that shadows a locked org Skill can still be saved; the Skills section shows it as conflicting. Delete asks for confirmation and removes the folder.
- **Files.** A saved Skill's files under `scripts/`, `references/` and `assets/` are listed with their sizes. Add files copies the chosen files into the selected folder (the registry's package limits apply, and names must be safe relative paths), and Remove deletes one file.
- **Save as skill.** A Thread's header offers Save as skill. The host collects the Thread's user and assistant messages, the tools its Runs called (runtime tools excluded) and the highest declared risk among them, and asks the main model, once, for JSON with a kebab-case name, a description and generalised instructions that use `{{parameter}}` placeholders for inputs. A deterministic pass then replaces what is still personal data in the description and the instructions: email addresses, Windows and UNC paths, and person names known from the Thread (the signed-in user's name and the display names of `Name <address>` pairs). Each distinct value becomes one parameter (`{{email_1}}`, `{{path_1}}`, `{{person_1}}`), listed under an Inputs heading. The editor opens with the draft and lists each replacement with Keep, which puts the original value back and removes its parameter. The model call is attributed to the Thread with the feature `skill_draft`.
- **Test runs.** Test runs the editor's current text, saved or not, against a sample input. The host creates a Thread with the system origin and the reference `{"skill_test": <name>}`; the Inbox and the Thread header show it as a Skill test. The draft is held in memory for that Thread, and the runtime's Skill lookups take the Thread, so in a test Thread the draft is the Skill of its name, listed to the agent, and started with `/name input` like an explicit invocation. The tool scope, the decision floor and approvals are the normal ones. Test runs are not counted as registry usage. After a restart the draft is gone and the Thread resolves Skills normally.
- **Publishing.** Publish on a private Skill offers the scopes in `policy.skills.publish` (default shared): shared with named user names, group with Groups chosen from the user's own, or org. The client zips the Skill folder with SKILL.md at the root and posts it to the registry's publish endpoint. The registry's answer is shown: published, or In review for org. Errors (name owned by another user, version exists, secret found) are shown with the registry's message. The Skills section shows the user's published versions and their states from the registry's `mine` list.

## Risks / Trade-offs

- [Malicious or harmful shared skills spread person to person] → Skills shared with named people or groups cannot escalate tools (D3). Org scope requires review and signing. Revocation is pushed, and a scan runs for group scope too.
- [A skill contains personal data from "save as skill"] → The draft step strips names, email addresses and file paths into parameters. The user reviews the draft before saving, and the scanner flags personal-data patterns before group or org publishing.
- [The review queue becomes a bottleneck] → Shared and group scopes need no review. Org review has a target turnaround, and the reviewer pool is managed per business unit.
- [The signing key is compromised] → The key lives in the control-plane secret store, and key rotation is supported by the client trusting the current and the next key. Revoking all skills signed with the old key is possible.
- [agentskills.io format drift] → Unknown frontmatter fields are preserved and ignored. A compatibility test suite runs against reference public skills.

## Migration Plan

Not applicable.

## Open Questions

- Who staffs the skill-reviewer pool in each business unit, and what is the review turnaround target? This affects process only, not the specs.
