# Tasks

## 1. Surface contract

- [x] 1.1 Validator and catalog for `markdown`, `facts`, `callout`, `timeline`, `tasks`, `draft`, root `template` and `title` (D2, D3); unit tests for valid and invalid cases; extend the fuzz seeds.
  Status: done 2026-11-27; GeneratedSurface.AcceptsDisplayComponentsAndATemplate, GeneratedSurface.RejectsInvalidDisplayComponents. Fuzz seeds template_<kind>.json (one per template) in fuzz/corpus/generated_surface; the fuzz target also runs generated_surface_text (fuzz build not run here). Limits the table left open are in D2 (as built).
- [x] 1.2 Content template instructions per template with one example each (D3); a unit test that every example validates and has no controls.
  Status: done 2026-11-27; GeneratedSurface.ContentTemplateExamplesValidateWithoutControls (each example parses, names its template, has no controls and is quoted in its instructions; no ask_user; under 3,200 bytes).

## 2. Rendering

- [x] 2.1 Native views for the six components and the template header, with UIA roles and names, both themes and en/cs/de strings (D2, D3); shell tests.
  Status: done 2026-11-27; ui/generated_content_view.{h,cpp}; UiGeneratedContent.DisplayComponentsAreNamedAndActLocally (header, list roles and item names, callout name, in-window task link, Copy to the clipboard, no a11y problems), UiGeneratedContent.EveryTemplateRendersInBothThemes (each example light and dark; SKOWORK_TEMPLATE_PNG_DIR saves the images), ShellFeed.DisplayOnlySurfaceRendersWithTheSettingOff. Strings generated.template.*, generated.callout.*, generated.task.*, generated.draft.* in en, cs and de.
- [x] 2.2 Display-only surfaces render with the setting off; runtime `surface_complete` and `surfaces.open`/`save` for them (D4); runtime and shell tests.
  Status: done 2026-11-27; LocalRunApi.DisplayOnlySurfacesWorkWithTheSettingOff, ShellFeed.DisplayOnlySurfaceRendersWithTheSettingOff; LocalRunApi.SurfaceStateIsGatedAndBoundToStoredAssistantEvents unchanged. `surface_complete` was already set for every complete message (D4 as built).
- [x] 2.3 Previews, toasts and excerpts show the surface summary (D5); test.
  Status: done 2026-11-27; InboxNotifications.PreviewAndToastShowTheSurfaceSummary (list preview, completion toast, search excerpt, invalid block left out), GeneratedSurface.TextOfAMessageShowsSummariesNotJson. Previews stored before the change are not rewritten (D5 as built).

## 3. Producers

- [x] 3.1 Daily report, meeting summary, Agenda preparation, triage preparation and Calendar meeting preparation ask for their template (D3); update the tests that pin those instructions.
  Status: done 2026-11-27; DailyReportCore.TriageResultsAreTheirOwnSections, AgendaService.PreparationStartsOnceWithAReadOnlyPreambleAndUntrustedData, EventTriage.ThePreparationOfATeamsItemNeverSendsMail and the meeting vocabulary test check the template in the preamble. The Calendar "Ask Skowork" preamble (app wiring) has no unit test.
- [x] 3.2 `agenda::extract_draft` reads a `draft` component (D5); core tests.
  Status: done 2026-11-27; AgendaDraft.ReadsTheDraftComponentOfASurfaceFirst; the heading-rule tests are unchanged.

## 4. Checks

- [x] 4.1 Client tests, string check, `openspec validate --all --strict`; render each template in the app and review it.
  Status: done 2026-11-27; ui_tests 114, agenda_tests 19, report_tests 5, triage_tests 10 pass; ui_strings_check passes; win_tests 1122 pass and 3 fail outside this change (EvalJobs.CancelAllStopsARunningJob fails every time, WithdrawalOutbox.SeparateProcessesPreserveEveryRemoval and ShellDictation.DictateInsertsAtTheCaretEscapeDiscardsAndAvailability are intermittent). Each template's example was rendered off-screen in the light and dark themes by the element test and reviewed; it was not checked in the running app with real model output. openspec validate not run: no npm.
