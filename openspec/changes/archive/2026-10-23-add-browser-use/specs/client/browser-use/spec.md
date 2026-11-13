# Spec Delta

## Purpose

Defines how the agent operates a dedicated Chrome browser to read and act in web applications, under policy control and without ever handling the user's passwords.

## ADDED Requirements

### Requirement: Dedicated Agent Browser Profile
The client SHALL run browser tools only in a dedicated, persistent Chrome profile stored under the data root. It MUST NOT attach to or modify the user's default Chrome profile.

#### Scenario: First browser use
- **WHEN** the agent first calls a browser tool
- **THEN** Chrome starts with the dedicated Skowork profile, and the user's own Chrome windows and profile are untouched

#### Scenario: Session persists
- **WHEN** the user signed in to a web application in the agent browser yesterday
- **THEN** today's browser session is still signed in, as long as the site's own session has not expired

### Requirement: Pipe-Only Control Channel
The client SHALL control Chrome only through a debugging pipe inherited by the Chrome process it started. It MUST NOT open or connect to a remote-debugging TCP port.

#### Scenario: No debugging port
- **WHEN** the agent browser is running
- **THEN** no listening TCP port for remote debugging exists on the machine

### Requirement: Chrome Availability And Policy Check
Before first use in a session, the client SHALL verify that a supported Chrome version is installed and that enterprise policy permits remote debugging for the dedicated profile. If either check fails, browser tools MUST be withdrawn and the reason shown.

#### Scenario: Debugging blocked by enterprise policy
- **WHEN** the managed Chrome policy disallows remote debugging
- **THEN** browser tools are not offered to the model, and Connections shows "Browser use blocked by Chrome policy" with the policy name

### Requirement: Page Snapshot With Element References
The client SHALL provide browser.snapshot. It returns the page's accessibility tree as compact text with a stable reference for each interactive element, which the action tools use to target elements. Page content in a snapshot is marked untrusted, with the page URL as provenance.

#### Scenario: Snapshot then click
- **WHEN** the agent takes a snapshot and then calls browser.click with an element reference from it
- **THEN** the referenced element is clicked, without coordinate guessing

#### Scenario: Stale reference
- **WHEN** the page changed and a reference no longer resolves
- **THEN** the action fails with a "stale reference" error that tells the agent to take a new snapshot

### Requirement: Screenshot For Vision
The client SHALL provide browser.screenshot, which returns a viewport or full-page image, sized as defined by client/model-access for the vision model role. Screenshots are held in memory for the duration of the Run only. They are written to the data root only if policy.tools.browser.retain_screenshots is true (default false).

#### Scenario: Vision fallback
- **WHEN** the agent calls browser.screenshot on a page rendered as a canvas
- **THEN** an image is returned for the vision model role

#### Scenario: Screenshots discarded
- **WHEN** a Run that took browser screenshots ends and retention is not allowed by policy
- **THEN** no screenshot remains in memory or in the data root

### Requirement: Browser Actions
The client SHALL provide these tools: browser.navigate, browser.click, browser.type, browser.select, browser.scroll, browser.wait, browser.tabs (list, open, switch, close) and browser.back. Each action reports the resulting URL and whether navigation occurred.

#### Scenario: Form fill
- **WHEN** the agent types into a text field and selects a dropdown value using element references
- **THEN** both values are set and the results confirm each action

### Requirement: Declared Risk Classes
Browser tools SHALL declare these Risk classes:
- read: browser.snapshot, browser.screenshot, browser.wait, browser.scroll, and browser.tabs list and switch;
- write-local: browser.navigate, browser.back, browser.click, browser.type, browser.select, and browser.tabs open and close, unless the next requirement makes them external;
- destructive: browser.eval.

#### Scenario: Plain click
- **WHEN** the agent clicks a link on the Run's starting site that does not submit anything
- **THEN** the call declares risk class write-local

### Requirement: Submitting Actions Are External
Browser actions that submit a form, or click a control that the page marks as submitting or confirming, SHALL declare risk class external. Actions on pages outside the Run's starting site SHALL also declare risk class external.

#### Scenario: Submit order
- **WHEN** the agent clicks a form's submit button
- **THEN** the call declares risk class external, and client/policy-enforcement decides whether approval is needed

### Requirement: Script Evaluation Is Destructive
The client SHALL provide browser.eval, which runs JavaScript in the page. It SHALL declare risk class destructive and is offered only when policy.tools.browser.allow_eval is true (default false).

#### Scenario: Eval disabled
- **WHEN** policy.tools.browser.allow_eval is false
- **THEN** browser.eval is not offered to the model

### Requirement: Egress Policy For Every Browser Request
The client SHALL submit every request the agent browser makes, including navigations, redirects and subresources, to the network egress rules of client/policy-enforcement. Requests that are not allowed MUST NOT be sent. A blocked navigation fails with the reason.

#### Scenario: Denied domain
- **WHEN** the agent navigates to a domain matched by policy.network.egress_deny
- **THEN** the navigation is blocked, no request is sent, and the tool result states that policy denied the domain

#### Scenario: Blocked subresource
- **WHEN** an allowed page loads a script from a domain that is not allowed
- **THEN** that request is not sent, and the page loads without it

### Requirement: No Password Entry By Agent
The agent MUST NOT type into password fields or other credential inputs, and MUST NOT choose a client certificate. When a page requires sign-in, a client certificate choice, MFA or a CAPTCHA, the client SHALL raise a client-raised Question through client/agent-runtime, with the choices Continue and Stop, asking the user to complete it in the agent browser. The Run resumes when the user chooses Continue.

#### Scenario: Login page
- **WHEN** the agent tries to type into a password field
- **THEN** the action is refused, the agent browser is brought to the front, and the Run is interrupted on a Question "Please sign in in the agent browser, then continue"

#### Scenario: Certificate choice
- **WHEN** a navigation does not complete because Chrome waits for the user to choose a client certificate for the site
- **THEN** in a visible window the Run is interrupted on the sign-in Question; in a headless browser the action fails with a reason that names the certificate choice

### Requirement: Organization Sign-In Through The Identity Provider
Organization web applications sign users in with OAuth 2.0 / OpenID Connect through the organization identity provider. The agent browser SHALL keep the identity provider's session in its dedicated profile. The client SHALL treat the identity provider's sign-in host, from the client's identity configuration, as a sign-in page (No Password Entry By Agent).

#### Scenario: Not signed in yet
- **WHEN** the agent opens an organization application and the identity provider shows its sign-in page
- **THEN** the Run is interrupted on the sign-in Question, and it resumes after the user signs in and chooses Continue

#### Scenario: Signed in once
- **WHEN** the user has signed in to the identity provider in the agent browser, and the agent then opens another application that uses it
- **THEN** the application loads signed in as the user, without a prompt

### Requirement: Visible Or Headless Mode
The agent browser SHALL run in a visible window during attended Runs. For Unattended runs it SHALL run headless only if policy.tools.browser.headless_unattended is true (default false); otherwise it uses a visible window.

#### Scenario: Unattended run with headless allowed
- **WHEN** a scheduled Run uses the browser and headless mode is allowed by policy
- **THEN** Chrome runs without a visible window

### Requirement: Browser Downloads Quarantined
Files the agent browser downloads SHALL be saved to the quarantine folder defined by client/core-tools, and are subject to the same rules.

#### Scenario: Report download
- **WHEN** the agent downloads a PDF report in the agent browser
- **THEN** the file is saved in the quarantine folder and the tool result returns its path

### Requirement: Browser Lifecycle
The client SHALL start the agent browser on first use and close it after policy.tools.browser.idle_timeout_minutes without browser tool calls (default 15). It SHALL never close it while the user is interacting with it.

#### Scenario: Idle close
- **WHEN** no browser tool has been called for 15 minutes and the user is not using the window
- **THEN** the agent browser is closed, and its profile is kept
