# Spec Delta

## ADDED Requirements

### Requirement: Mail Backend Selection
The client SHALL reach the user's mailbox and calendar through exactly one backend, chosen by policy.mail.backend with the values `auto` (default), `graph` and `outlook`. With `auto`, the backend is Graph when policy.mail.oauth is complete, and otherwise the Local Outlook backend when classic Outlook is installed for the user. When the chosen backend changes, the client SHALL clear the local mail and calendar copy and fill it again without publishing events for items that existed before.

#### Scenario: No app registration configured
- **WHEN** policy.mail.backend is auto, policy.mail.oauth.client_id is empty and classic Outlook is installed
- **THEN** mail and calendar use the Local Outlook backend, and the Mail and calendar settings section names Outlook as the backend

#### Scenario: Graph configured later
- **WHEN** policy.mail.oauth becomes complete while the Local Outlook backend is in use and policy.mail.backend is auto
- **THEN** the client switches to Graph, refills the local copy, and no "mail received" or "event created" events are published for the refilled items

#### Scenario: Neither backend possible
- **WHEN** policy.mail.backend is outlook and classic Outlook is not installed
- **THEN** mail and calendar tools, Sources and Triggers are unavailable, and the settings section says that classic Outlook was not found

### Requirement: Local Outlook Backend
With the Local Outlook backend, the client SHALL access the mailbox and calendar of the user's default classic Outlook profile on the same device, through Outlook's object model, and SHALL provide every behaviour of this capability with it. That includes the mail and calendar tools and their risk classes, the send approval floor, sensitivity labels, exclusions, the sync window, sync, the event feed and offline behaviour. It SHALL NOT open a network connection of its own for mail or calendar.

#### Scenario: Same tool behaviour
- **WHEN** the agent calls mail.search, mail.read or calendar.list on the Local Outlook backend
- **THEN** the results have the same shape, exclusions and "withheld (sensitivity label)" placeholders as on Graph

#### Scenario: Labelled item in Outlook
- **WHEN** a message in Outlook carries a sensitivity label listed in policy.mail.excluded_labels
- **THEN** the message is withheld exactly as on Graph

#### Scenario: Recurring meeting
- **WHEN** the calendar holds a weekly meeting
- **THEN** each occurrence in the sync window is a separate event with its own start and end, linked to its series

### Requirement: Outlook Availability
The Local Outlook backend SHALL attach only to an Outlook that is already running for the signed-in user, unless the user allows Skowork to start Outlook (default off). While Outlook is not running, or does not answer within 60 seconds, mail and calendar calls SHALL behave as offline per Offline Mail Behaviour, and sync SHALL retry at its next interval.

#### Scenario: Outlook closed
- **WHEN** the agent calls mail.search while Outlook is not running and the user has not allowed Skowork to start it
- **THEN** results come from the local copy, labelled with the time of the last successful sync, and Outlook is not started

#### Scenario: Outlook shows a dialog
- **WHEN** Outlook rejects calls because it is showing a modal dialog
- **THEN** the client retries with backoff and reports the delay in the Run's Activity log, as for throttling

### Requirement: Sending Through Outlook
On the Local Outlook backend, mail.send, calendar.create and calendar.respond SHALL send through the user's Outlook, so the message leaves from the user's Outlook account and is filed in its Sent Items. An Outlook security prompt that the user declines SHALL fail the call with "access denied". An Outlook prompt never replaces the approval required by Send Always Requires Approval.

#### Scenario: Approved send
- **WHEN** the user approves a mail.send call on the Local Outlook backend
- **THEN** the message is sent by Outlook and appears in Outlook's Sent Items

#### Scenario: Prompt declined
- **WHEN** Outlook asks the user whether a program may send mail and the user denies it
- **THEN** the tool call fails with "access denied" and nothing is sent
