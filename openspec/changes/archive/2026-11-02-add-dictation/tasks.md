## 1. Host

- [x] 1.1 policy.dictation.mode in the control plane's keys and the client's schema fixture (D4). Verify with the policy tests
- [x] 1.2 DictationService with dictation.start, stop and cancel, the 5-minute limit, leftover cleanup, and app.status.dictation (D1, D2). Verify with tests using a fake recorder and the test speech engine: text returned, file deleted after stop and cancel, refusals for policy off and a running meeting capture

## 2. Desktop app

- [x] 2.1 The Dictate button in the Thread, new Thread and Chat composers, with recording, transcribing and insertion at the caret (D3); strings in en, de, cs. Verify with hidden-window UI tests and an a11y check

- [x] 2.2 Dictation language (D5): the setting in Settings > General (Automatic, cs, de, en, pl, sk), the language or fallback in dictation.stop, and the fallback in stt.transcribe; strings in en, de, cs. Verify with tests: a chosen language is sent; with Automatic an unsupported detection falls back to the window language; the setting's choice in the Settings page

## 3. Acceptance

- [x] 3.1 On the demo client with the sherpa-onnx engine: dictate a sentence in English and in Czech into a Thread's composer and send it; cancel one with Escape and check the audio is gone. Record the result in design.md
- [x] 3.2 Run `openspec validate --all --strict`
