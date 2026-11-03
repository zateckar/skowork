---
name: supplier-triage
description: Triage supplier mail
version: 1.2.0
tools: [mail.search, mail.draft]
risk: write-local
network: none
requires_mcp:
  - jira-corp
interrupt_on:
  mail.draft: [approve, edit, reject]
  shell.run: true
triggers:
  - kind: schedule
    cron: "0 8 * * 1"
notes: |
  # heading
  text
meta: {a: [1, 2], b: 'x'}
---
# Steps
Do it.
