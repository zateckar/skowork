# Spec Delta

## ADDED Requirements

### Requirement: Triage Thread Badges
A Thread created by client/event-triage SHALL show its topic category badge instead of the system origin badge, in the list and in the Thread header. The category badge SHALL read Request, Approval, Question, Meeting, Deadline, Decision or Update. Its status SHALL be the single "Needs your action" badge: shown while the Thread has an open Action item, and for a triage action Thread that has not had a Run yet. A triage Thread SHALL NOT show a second badge with the same meaning.

#### Scenario: Approval request in the list
- **WHEN** triage created a Thread for an approval request, and its preparation waits on a Question card
- **THEN** its list entry shows the badges "Approval" and "Needs your action", and neither a "Skowork" badge nor an "Action required" badge

#### Scenario: Reply sent
- **WHEN** the user approved the reply in a triage Thread and its Run ended
- **THEN** its list entry shows the category badge and no "Needs your action" badge

### Requirement: Collapsed Source Message
When a Run starts from untrusted source content (a mail, a calendar event, a Teams message or a Remote task request), the Thread feed SHALL show that first message as a collapsed card. The card's header names the kind of source, and its body is the source content only, without the client's instructions to the model. The user SHALL be able to expand it. The rest of the feed, starting with the agent's answer, SHALL follow without the source taking up the view.

#### Scenario: Prepared mail Thread opened
- **WHEN** the user opens a Thread prepared from a long mail
- **THEN** a collapsed "Source: Mail" card is shown first, and the summary is visible without scrolling through the mail
