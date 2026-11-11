// Built-in tool IDs are independent of the policy that happens to be in force.
// MCP tools are discovered separately and custom patterns remain valid.
export const BUILT_IN_TOOL_IDS = [
  'ask_user', 'find_tools', 'read_attachment', 'delegate', 'use_skill',
  'apps.create', 'apps.list', 'apps.logs', 'apps.open', 'apps.stop', 'apps.delete',
  'browser.snapshot', 'browser.screenshot', 'browser.navigate', 'browser.click', 'browser.type', 'browser.select',
  'browser.scroll', 'browser.wait', 'browser.tabs', 'browser.back', 'browser.eval',
  'computer.observe', 'computer.launch', 'computer.screenshot', 'computer.click', 'computer.type', 'computer.keys',
  'computer.scroll', 'computer.drag', 'computer.select',
  'knowledge.search', 'knowledge.entity', 'knowledge.related', 'knowledge.timeline', 'knowledge.remember',
  'mail.search', 'mail.read', 'mail.draft', 'mail.send', 'mail.move', 'mail.delete',
  'calendar.list', 'calendar.get', 'calendar.find_free_time', 'calendar.create', 'calendar.respond',
  'messages.find_user', 'messages.list_chats', 'messages.read', 'messages.send',
  'tasks.list', 'tasks.create', 'tasks.update', 'tasks.delete', 'tasks.run_now',
  'files.read', 'files.list', 'files.glob', 'files.grep', 'files.write', 'files.edit',
  'image.generate', 'shell.run', 'web.fetch',
  'skills.save',
] as const;

// Computer actions have dynamic risk: the concrete target and action may raise the decision to destructive.
// Keep this separate from effective policy, which also depends on the user, device, group and guardrails.
export const COMPUTER_TOOL_DETAILS: Record<string, { description: string; owner: string; declared_risk: string; prerequisite: string; caveat: string }> = {
  'computer.observe': { description: 'Read the desktop accessibility tree.', owner: 'client/computer-use', declared_risk: 'read', prerequisite: 'Computer mode must allow use.', caveat: 'The target must be reachable and permitted.' },
  'computer.launch': { description: 'Start an executable and target its window.', owner: 'client/computer-use', declared_risk: 'external', prerequisite: 'Computer mode must allow use.', caveat: 'The executable and target must be permitted.' },
  'computer.screenshot': { description: 'Capture the target window or screen.', owner: 'client/computer-use', declared_risk: 'read', prerequisite: 'Computer mode and vision perception must allow use.', caveat: 'The target must be reachable and permitted.' },
  ...Object.fromEntries(Object.entries({
    'computer.click': 'Click a desktop element or screenshot point.',
    'computer.type': 'Type text into a desktop field.',
    'computer.keys': 'Press keys in a desktop window.',
    'computer.scroll': 'Scroll a desktop window or element.',
    'computer.drag': 'Drag between desktop elements or points.',
    'computer.select': 'Choose a visible option in a desktop control.',
  }).map(([id, description]) => [id, { description, owner: 'client/computer-use', declared_risk: 'destructive',
    prerequisite: 'Computer mode must allow use.', caveat: 'Risk depends on the action and target; unattended use has additional restrictions.' }])),
};

// These values describe registration, not an effective permission. Availability and
// policy are evaluated separately in the desktop client for each invocation.
type ToolDetail = (typeof COMPUTER_TOOL_DETAILS)[string];
function familyDetails(owner: string, prerequisite: string, caveat: string,
  entries: Record<string, [description: string, declaredRisk: string]>): Record<string, ToolDetail> {
  return Object.fromEntries(Object.entries(entries).map(([id, [description, declared_risk]]) =>
    [id, { description, owner, declared_risk, prerequisite, caveat }]));
}

export const BUILT_IN_TOOL_DETAILS: Record<string, ToolDetail> = {
  ...COMPUTER_TOOL_DETAILS,
  ...familyDetails('client/agent-runtime', 'The runtime must offer the tool for this run.', 'Actions performed by a delegated run or skill are decided separately.', {
    'ask_user': ['Ask the user for information or a decision.', 'read'],
    'find_tools': ['Search available tools by name and description.', 'read'],
    'read_attachment': ['Read part of an attached tool result.', 'read'],
    'delegate': ['Delegate an independent task to a subagent.', 'read'],
    'use_skill': ['Run an available skill as a subagent.', 'read'],
  }),
  ...familyDetails('client/local-apps', 'Local apps must be available.', 'Policy and app-specific confirmation still apply.', {
    'apps.create': ['Create a local app or change its manifest.', 'write_local'],
    'apps.list': ['List local apps and their status.', 'read'],
    'apps.logs': ['Read the end of a local app log.', 'read'],
    'apps.open': ['Start a local app and open it in the browser.', 'write_local'],
    'apps.stop': ['Stop a running local app.', 'write_local'],
    'apps.delete': ['Delete a local app, its folder and data.', 'destructive'],
  }),
  ...familyDetails('client/knowledge-graph', 'The knowledge service must be available.', 'Results depend on the sources and sharing available to the user.', {
    'knowledge.search': ['Search available knowledge and return source passages.', 'read'],
    'knowledge.entity': ['Read an entity profile and its sourced facts.', 'read'],
    'knowledge.related': ['Find entities related to an entity.', 'read'],
    'knowledge.timeline': ['Read dated facts and items for an entity or topic.', 'read'],
    'knowledge.remember': ['Remember a fact or preference stated by the user.', 'write_local'],
  }),
  ...familyDetails('client/scheduler', 'The scheduler must be running.', 'Policy and automation settings still apply.', {
    'tasks.list': ['List automations and their runs.', 'read'],
    'tasks.create': ['Create an automation.', 'write_local'],
    'tasks.update': ['Change an automation.', 'write_local'],
    'tasks.delete': ['Delete an automation and its history.', 'write_local'],
    'tasks.run_now': ['Run an automation now.', 'write_local'],
  }),
  ...familyDetails('client/core-tools', 'The requested path must be permitted.', 'Read and write path restrictions apply separately.', {
    'files.read': ['Read a text file or extract document text.', 'read'],
    'files.list': ['List files and folders.', 'read'],
    'files.glob': ['Find files and folders by name pattern.', 'read'],
    'files.grep': ['Search text file contents by regular expression.', 'read'],
    'files.write': ['Create or replace a file.', 'write_local'],
    'files.edit': ['Replace a unique text span in a file.', 'write_local'],
  }),
  ...familyDetails('client/mail-calendar', 'The mail service and the required account permissions must be available.', 'Policy, sensitivity restrictions and service permissions still apply.', {
    'mail.search': ['Search the user’s mail.', 'read'],
    'mail.read': ['Read a message, conversation or attachment.', 'read'],
    'mail.draft': ['Create or update a draft without sending it.', 'external'],
    'mail.send': ['Send email after showing the message for user approval.', 'external'],
    'mail.move': ['Move a message to another folder.', 'destructive'],
    'mail.delete': ['Move a message to Deleted Items.', 'destructive'],
    'calendar.list': ['List calendar events in a time range.', 'read'],
    'calendar.get': ['Read an event and its attendees.', 'read'],
    'calendar.find_free_time': ['Find a time when attendees are free.', 'read'],
    'calendar.create': ['Create or update an event, potentially inviting attendees.', 'external'],
    'calendar.respond': ['Respond to a meeting invitation.', 'external'],
  }),
  ...familyDetails('client/browser-use', 'The agent browser must be available; evaluation also requires explicit enablement.', 'Risk can depend on the action and target, and network policy still applies.', {
    'browser.snapshot': ['Read the current page accessibility tree.', 'read'],
    'browser.screenshot': ['Capture the current page.', 'read'],
    'browser.navigate': ['Open a URL in the agent browser.', 'external'],
    'browser.click': ['Click an element on the page.', 'external'],
    'browser.type': ['Type into a page field.', 'external'],
    'browser.select': ['Choose an option on the page.', 'external'],
    'browser.scroll': ['Scroll the page or an element.', 'read'],
    'browser.wait': ['Wait for page text, time or user action.', 'read'],
    'browser.tabs': ['List, open, switch or close task tabs.', 'external'],
    'browser.back': ['Go back in the current tab.', 'external'],
    'browser.eval': ['Run a JavaScript expression in the page.', 'destructive'],
  }),
  ...familyDetails('client/user-messaging', 'The messaging service must be available.', 'Sending while offline queues the message for reconnection.', {
    'messages.find_user': ['Find colleagues in the user directory.', 'read'],
    'messages.list_chats': ['List the user’s chats.', 'read'],
    'messages.read': ['Read recent messages from a chat.', 'read'],
    'messages.send': ['Send a message to a chat or colleague.', 'external'],
  }),
  ...familyDetails('client/core-tools', 'The relevant service and policy must permit the request.', 'Resource and network restrictions apply to each request.', {
    'web.fetch': ['Fetch a web page or file over HTTP or HTTPS.', 'read'],
    'image.generate': ['Generate images with the organization image model.', 'read'],
  }),
  ...familyDetails('client/core-tools', 'The tool sandbox and PowerShell must be available.', 'The displayed risk is the worst case; command contents and policy determine the risk of each call.', {
    'shell.run': ['Run a PowerShell command in the tool sandbox.', 'destructive'],
  }),
  ...familyDetails('client/agent-skills', 'Offered only in attended top-level runs.', 'The saved Skill is private; sharing happens only in its Publish dialog.', {
    'skills.save': ['Save or update a private Skill the agent drafted.', 'write_local'],
  }),
};
