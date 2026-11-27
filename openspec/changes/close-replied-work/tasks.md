# Tasks

## 1. Mail

- [x] 1.1 `mail.sent` from the sync for new non-draft messages in Sent Items outside backfill (D2); a reply lookup over the local copy (D1); unit tests.
  Status: done 2026-11-27; OutlookBackend.MailTheUserSentIsAnnouncedAsSentOutsideBackfill (mail.sent, no event for backfill or drafts, MailStore::user_replied). The Graph-mock MailSync tests skip here (no node). Sent Mail Event added to client/mail-calendar (delta in this change).

## 2. Agenda

- [x] 2.1 Close open items on a reply and skip answered mail at detection (D2, D3); cancel the preparation Run and archive its Thread; the `replied` notice; service tests.
  Status: done 2026-11-27; AgendaService.AReplyInTheConversationClosesTheItemAndItsPreparation, AgendaService.AnItemWithANewerNonMailSourceStaysOpen, AgendaService.MailAnsweredBeforeDetectionAddsNoItemButANewQuestionDoes.

## 3. Triage

- [x] 3.1 Migration 2900 with suggestion `replied` (D4); close on a reply and skip answered mail before creating a Thread; banner status text; triage tests.
  Status: done 2026-11-27; EventTriage.ARepliedItemIsClosedItsRunCancelledAndItsThreadArchived, EventTriage.AReplyOnlyClosesMailReceivedBeforeIt, EventTriage.MailAnsweredBeforeTriageGetsNoThreadButANewQuestionDoes, EventTriage.MigrationAddsRepliedAndKeepsRows. The banner shows thread.suggestion.status_replied; No action needed stays limited to open and started.

## 4. Teams

- [x] 4.1 `TeamsService::user_replied` (D5); triage and Agenda close and skip on Teams replies; tests for chat, channel thread and answered-before-triage.
  Status: done 2026-11-27; TeamsService.UserRepliedIsALaterOwnMessageAndInAChannelTheSameThread, EventTriage.ATeamsReplyClosesTheTeamsThreadItAnswered, EventTriage.ATeamsMessageAnsweredBeforeTriageGetsNoThread, AgendaService.ATeamsReplyClosesTheItemAndAnsweredTeamsMessagesAddNone. The banner text thread.suggestion.status_replied no longer says "mail".

## 5. Checks

- [x] 5.1 en, cs and de strings; client tests; `openspec validate --all --strict`.
  Status: done 2026-11-27; ui_strings_check, agenda_tests 19, triage_tests 10, teams_tests 17, ui_tests 114 pass; win_tests 1126 pass, 3 fail in the full run and pass alone (MeetingService.IndicatorStopEndsTheCaptureWithinTwoSeconds, Replay.BusyAndCancelKeepsWritten, WithdrawalOutbox.SeparateProcessesPreserveEveryRemoval). openspec validate not run: no npm.
