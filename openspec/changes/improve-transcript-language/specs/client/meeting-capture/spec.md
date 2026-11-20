# Spec Delta

## ADDED Requirements

### Requirement: Speech Decoded In Context
The client SHALL decode speech of one track that is separated by pauses shorter than 1 second together, as one piece of up to 20 seconds, so that no word or short phrase is recognised on its own while other speech of the same track lies within 1 second of it. The transcript keeps a segment for each part of that piece with its own time.

#### Scenario: Short reply inside a turn
- **WHEN** a remote participant says "Yes." and continues 0.6 seconds later with a longer sentence
- **THEN** "Yes." and the sentence are recognised together, in the meeting's language

#### Scenario: Isolated reply
- **WHEN** the user says "Okay." with no other speech on the local track within 1 second
- **THEN** it is recognised on its own

### Requirement: Transcript Keeps To The Meeting Language
The client SHALL decode again, with the meeting's language set, every segment whose text is in another script than the meeting's language and every segment of at most five words that is recognised in another language than the meeting's. Segments of six or more words in another supported language are kept as recognised.

#### Scenario: Czech sentence written in Cyrillic
- **WHEN** a Czech meeting has a segment that Parakeet writes in Cyrillic
- **THEN** the segment is decoded again in Czech, and the transcript shows Czech text or "(unclear)", never Cyrillic

#### Scenario: Short English fragment in a Czech meeting
- **WHEN** a two-word fragment of a Czech meeting is recognised as English words
- **THEN** it is decoded again in Czech, and shows as Czech text or "(unclear)"

#### Scenario: English contribution in a Czech meeting
- **WHEN** a participant of a Czech meeting says a full English sentence of ten words
- **THEN** the sentence stays in the transcript in English

### Requirement: Unclear Speech
When a segment decoded again in the meeting's language is still empty or in another script or language, the transcript SHALL show it as "(unclear)" at its time, and it is left out of the meeting outputs and the knowledge item.

#### Scenario: Still not the meeting's language
- **WHEN** a Cyrillic segment of a Czech meeting is decoded again in Czech and the result is empty
- **THEN** the transcript shows "(unclear)" at that time, and the summary and the knowledge item do not contain it

### Requirement: Meeting Language Choice
The user SHALL be able to choose the language of manual captures, Automatic or one of the supported transcript languages, in the Record meeting list, and to transcribe a finished meeting again in another language from its Thread while its audio is kept. The choice is remembered for later captures, and a chosen language is used without detection. Transcribing again replaces the transcript and the knowledge item, and starts a new summary.

#### Scenario: Language chosen before recording
- **WHEN** the user chooses Čeština in the Record meeting list and records a meeting
- **THEN** the meeting is transcribed in Czech without language detection

#### Scenario: Transcribe again
- **WHEN** a meeting was detected as English, its audio is kept, and the user chooses to transcribe it again in Czech
- **THEN** the transcript and the knowledge item are replaced by the Czech ones, and a Czech summary is written

#### Scenario: Audio no longer kept
- **WHEN** a meeting's audio was deleted after transcription
- **THEN** its Thread offers no transcribing again
