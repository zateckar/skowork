# Proposal

## Why

On 2026-11-02 the product owner asked for the microphone as an input: the client already transcribes speech on the device for meetings (client/meeting-capture), yet users have to type every request to the agent and every Chat message.

## What Changes

- **Dictate** (a microphone button) in the composers of a Thread, of a new Thread and of a Chat.
  - Clicking it starts recording the default microphone. The button turns into Stop with the elapsed time.
  - Clicking Stop transcribes the recording on the device, with the speech engine of client/meeting-capture (Czech, German, English and the other languages it supports, detected automatically). The text is inserted into the composer at the caret. Nothing is sent: the user reads, edits and sends as usual.
  - Escape, or Cancel, discards the recording. A recording stops on its own after 5 minutes.
- **Privacy.** The audio never leaves the device. It is written encrypted to a temporary file and deleted once transcribed or discarded. Nothing is recorded unless the user clicked Dictate, and the button shows that recording is on.
- **Policy.** policy.dictation.mode (on or off, default on) lets an administrator switch Dictation off. The button is then not shown.
- When the speech models or a microphone are missing, the button is disabled and its tooltip says why.

Out of scope:
- Live text while speaking (the text comes after Stop).
- Voice commands, and speaking replies aloud.
- Dictation in other text fields (dialogs, settings, the Skill editor).

## Capabilities

### New Capabilities
- `client/dictation`: Dictation in Composers, Dictation Privacy, Dictation Availability.

### Modified Capabilities
- `client/policy-enforcement`: Dictation Policy (added).

## Impact

- **Roadmap:** 28, after fold-chats-into-inbox. Depends on add-meeting-capture (microphone capture, encrypted tracks, the stt worker) and add-native-ui (the composers).
- **Glossary:** "Dictation" is added.
- **Code:**
  - client/src/app: the dictation service (microphone capture into an encrypted track, transcription in the stt worker, deletion), the RPCs dictation.start, dictation.stop and dictation.cancel, and its availability in app.status;
  - client/src/shell: the Dictate button in the composers;
  - control-plane/src/policy: the key policy.dictation.mode, and the client's schema fixture;
  - strings in en, de, cs.
- **Tests:** the service with a fake recorder and the test speech engine; UI tests of the button; the policy key.
- **Data:** temporary encrypted audio under the data root's capture folder, deleted after use.
