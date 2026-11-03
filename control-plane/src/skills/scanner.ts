// The automated Skill scan (control-plane/skill-registry Automated Skill Scan, Scan Findings; add-skills design D4, D6).
// Static checks only. A secret finding rejects the version in every scope; every other finding is advisory for review.
// Findings never carry the value of a secret.
import type { Manifest, SkillPackage } from './package.ts';

export type Severity = 'critical' | 'high' | 'medium' | 'low' | 'info';

export interface Finding {
  kind: 'secret' | 'risky-command' | 'injection' | 'hidden-characters' | 'personal-data';
  severity: Severity;
  file: string;
  line: number;
  rule: string;
  message: string;
}

export interface ScanResult {
  findings: Finding[];
  /** Commands that scripts run, with where they appear. */
  commands: { command: string; file: string; line: number }[];
  urls: { url: string; file: string; line: number }[];
  declared: { tools: string[]; requires_mcp: string[]; risk: Manifest['risk']; network: Manifest['network'] };
}

const TEXT = /\.(md|markdown|txt|ps1|psm1|psd1|py|js|mjs|cjs|ts|cmd|bat|sh|json|ya?ml|toml|ini|cfg|xml|csv|html?|css|sql)$/i;
const SCRIPT = /^scripts\/.+\.(ps1|psm1|py|js|mjs|cjs|cmd|bat|sh)$/i;

// A built-in subset of well-known token formats (after the gitleaks ruleset), plus generic key assignments.
const SECRET_RULES: { rule: string; re: RegExp }[] = [
  { rule: 'private-key', re: /-----BEGIN (?:RSA |EC |DSA |OPENSSH |PGP |ENCRYPTED )?PRIVATE KEY(?: BLOCK)?-----/ },
  { rule: 'aws-access-key', re: /\b(?:AKIA|ASIA)[0-9A-Z]{16}\b/ },
  { rule: 'github-token', re: /\b(?:gh[pousr]_[A-Za-z0-9]{36}|github_pat_[A-Za-z0-9_]{60,})\b/ },
  { rule: 'gitlab-token', re: /\bglpat-[A-Za-z0-9_-]{20}\b/ },
  { rule: 'slack-token', re: /\bxox[abposr]-[A-Za-z0-9-]{10,}\b/ },
  { rule: 'openai-key', re: /\bsk-(?:proj-|svcacct-)?[A-Za-z0-9_-]{32,}\b/ },
  { rule: 'google-api-key', re: /\bAIza[0-9A-Za-z_-]{35}\b/ },
  { rule: 'azure-storage-key', re: /AccountKey=[A-Za-z0-9+/]{40,}={0,2}/ },
  { rule: 'jwt', re: /\beyJ[A-Za-z0-9_-]{10,}\.eyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\b/ },
  { rule: 'basic-auth-url', re: /\bhttps?:\/\/[^\s:/@]+:[^\s:/@]{4,}@[^\s/]+/ },
  {
    rule: 'generic-secret',
    re: /\b(?:api[_-]?key|apikey|secret|client[_-]?secret|access[_-]?token|auth[_-]?token|token|password|passwd|pwd)\b["']?\s*(?:[:=]|-eq)\s*["']([^"'\s$<>{}%]{8,})["']/i,
  },
];

const PLACEHOLDER = /^(?:x+|\*+|\.+|changeme|example|placeholder|your[_-]?.*|redacted|dummy|test|<.*>)$/i;

const RISKY: { rule: string; re: RegExp; message: string }[] = [
  { rule: 'download-execute', re: /\b(?:iex|Invoke-Expression)\b[^\n]*\b(?:iwr|irm|Invoke-WebRequest|Invoke-RestMethod|DownloadString|curl|wget)\b|\b(?:iwr|irm|Invoke-WebRequest|Invoke-RestMethod|DownloadString)\b[^\n]*\|\s*(?:iex|Invoke-Expression)\b|curl[^\n|]*\|\s*(?:ba|z)?sh\b/i, message: 'downloads and executes code' },
  { rule: 'encoded-command', re: /\s-(?:e|ec|enc|encodedcommand)\s+[A-Za-z0-9+/=]{16,}/i, message: 'runs an encoded command' },
  { rule: 'recursive-delete', re: /\bRemove-Item\b[^\n]*-Recurse|\brm\s+-[a-z]*r[a-z]*f|\brmdir\s+\/s|\brd\s+\/s|\bdel\s+\/s/i, message: 'deletes recursively' },
  { rule: 'execution-policy', re: /\bSet-ExecutionPolicy\b|-ExecutionPolicy\s+(?:Bypass|Unrestricted)/i, message: 'changes the execution policy' },
];

const INJECTION: { rule: string; re: RegExp }[] = [
  { rule: 'ignore-instructions', re: /\b(?:ignore|disregard|forget|override)\b[^\n.]{0,30}\b(?:previous|prior|above|earlier|all|system|your)\b[^\n.]{0,20}\b(?:instructions|rules|prompts?|guidelines)\b/i },
  { rule: 'role-reassignment', re: /\byou are now\b|\bfrom now on,? you\b|\bnew instructions:/i },
  { rule: 'system-prompt', re: /\b(?:reveal|print|show|repeat|output)\b[^\n.]{0,30}\bsystem prompt\b/i },
  { rule: 'conceal-from-user', re: /\b(?:do not|don't|never)\b[^\n.]{0,20}\b(?:tell|inform|show|mention to|ask)\b[^\n.]{0,10}\bthe user\b|\bwithout (?:asking|telling|informing) the user\b/i },
  { rule: 'exfiltration', re: /\b(?:exfiltrate|send (?:all |the )?(?:credentials|passwords|tokens|secrets))\b/i },
  { rule: 'approval-claim', re: /\b(?:the user|admin(?:istrator)?)\b[^\n.]{0,20}\b(?:already )?(?:approved|authori[sz]ed|pre-approved)\b/i },
];

// Zero-width characters, bidirectional controls and tag characters.
const HIDDEN = /[\u200B-\u200F\u2060-\u2064\u202A-\u202E\u2066-\u2069\uFEFF]|[\u{E0000}-\u{E007F}]/u;

const EMAIL = /\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}\b/;
const EXAMPLE_DOMAIN = /@(?:[a-z0-9-]+\.)*(?:example\.(?:com|org|net)|example|test|invalid|localhost)$/i;
const PROFILE_PATH = /\b[A-Za-z]:\\Users\\(?!Public\\|Default\\|All Users\\|%|\$|<|\{)([^\\\s"'<>]+)\\/i;
const URL_RE = /\b(?:https?|wss?|ftp):\/\/[^\s"'<>)\]`]+/gi;

/** Commands in a script line: PowerShell Verb-Noun cmdlets and the first word of a statement. */
function commandsIn(line: string): string[] {
  const out = new Set<string>();
  const code = line.replace(/(^|\s)#.*$/, '').trim();
  if (!code || code.startsWith('<#') || code.startsWith('REM ') || code.startsWith('::')) return [];
  for (const m of code.matchAll(/\b([A-Z][a-z]+-[A-Z][A-Za-z]+)\b/g)) out.add(m[1]!);
  for (const stmt of code.split(/[|;]|&&|\|\|/)) {
    const first = /^\s*&?\s*["']?([A-Za-z][\w.-]*(?:\.exe)?)["']?/.exec(stmt)?.[1];
    if (!first) continue;
    if (/^(?:if|else|elseif|foreach|for|while|do|switch|function|param|return|try|catch|finally|begin|process|end|import|from|def|class|const|let|var|echo|set|then|fi|done|in)$/i.test(first)) continue;
    if (/^[A-Z][a-z]+-[A-Z]/.test(first) || /^[a-z][\w.-]*$/i.test(first)) out.add(first);
  }
  return [...out].filter((c) => !/^\$/.test(c) && !/^(true|false|null)$/i.test(c));
}

export function scanPackage(pkg: SkillPackage): ScanResult {
  const findings: Finding[] = [];
  const commands: ScanResult['commands'] = [];
  const urls: ScanResult['urls'] = [];
  const seenCmd = new Set<string>();
  for (const f of pkg.files) {
    if (!TEXT.test(f.path) && !/^(SKILL\.md|README|LICEN[CS]E)$/i.test(f.path)) continue;
    const text = f.data.toString('utf8');
    const lines = text.split(/\r\n?|\n/);
    const isScript = SCRIPT.test(f.path);
    const isInstructions = f.path === 'SKILL.md' || f.path.startsWith('references/');
    lines.forEach((line, i) => {
      const at = { file: f.path, line: i + 1 };
      for (const r of SECRET_RULES) {
        const m = r.re.exec(line);
        if (!m) continue;
        if (r.rule === 'generic-secret' && PLACEHOLDER.test(m[1] ?? '')) continue;
        findings.push({ kind: 'secret', severity: 'critical', ...at, rule: r.rule, message: `A secret (${r.rule}) appears in ${f.path} at line ${i + 1}.` });
        break;
      }
      for (const m of line.matchAll(URL_RE)) {
        const url = m[0].replace(/[.,;:]+$/, '');
        if (!urls.some((u) => u.url === url && u.file === f.path)) urls.push({ url, ...at });
      }
      if (isScript) {
        for (const r of RISKY) if (r.re.test(line)) findings.push({ kind: 'risky-command', severity: 'high', ...at, rule: r.rule, message: `The script ${r.message}.` });
        for (const c of commandsIn(line)) {
          const key = `${f.path}\u0000${c.toLowerCase()}`;
          if (!seenCmd.has(key)) (seenCmd.add(key), commands.push({ command: c, ...at }));
        }
      }
      if (isInstructions) {
        for (const r of INJECTION) if (r.re.test(line)) findings.push({ kind: 'injection', severity: 'medium', ...at, rule: r.rule, message: 'The text looks like an instruction to the agent that overrides its rules.' });
      }
      if (HIDDEN.test(f.path === 'SKILL.md' && i === 0 ? line.replace(/^\uFEFF/, '') : line)) {
        findings.push({ kind: 'hidden-characters', severity: 'medium', ...at, rule: 'hidden-characters', message: 'The line contains invisible or bidirectional control characters.' });
      }
      const email = EMAIL.exec(line);
      if (email && !EXAMPLE_DOMAIN.test(email[0])) findings.push({ kind: 'personal-data', severity: 'low', ...at, rule: 'email-address', message: 'An email address appears; consider a parameter.' });
      if (PROFILE_PATH.test(line)) findings.push({ kind: 'personal-data', severity: 'low', ...at, rule: 'user-profile-path', message: 'A path inside a user profile appears; consider a parameter.' });
    });
  }
  const m = pkg.manifest;
  return { findings, commands, urls, declared: { tools: m.tools, requires_mcp: m.requires_mcp, risk: m.risk, network: m.network } };
}
