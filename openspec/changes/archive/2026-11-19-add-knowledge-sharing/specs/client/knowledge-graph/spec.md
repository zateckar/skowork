# Spec Delta

## MODIFIED Requirements

### Requirement: Local-Only Knowledge Store
The client SHALL keep all knowledge data (chunks, indexes, embeddings, entities, relations and facts) only in the user's data root. It MUST NOT upload knowledge data to the control plane or to any other service, with two exceptions: model requests sent through client/model-access (LLM context, embedding and reranking), which return results but store nothing on the server, and the text of items the user explicitly shares under Sharing Knowledge Items, which goes only to control-plane/team-knowledge.

#### Scenario: No knowledge sync
- **WHEN** the knowledge store contains indexed items and the user has shared none of them
- **THEN** no request from the client to any control-plane service other than the LLM gateway contains chunk text, embeddings or extracted facts

#### Scenario: Only shared items leave the laptop
- **WHEN** the user shares one document of an indexed folder
- **THEN** the text of that document is sent to control-plane/team-knowledge, and nothing of the other documents, entities or facts is sent

### Requirement: File Ingestion Scope
The files Source SHALL index only folders and individual documents that are both permitted by policy.filesystem read rules and selected by the user. Supported types are docx, xlsx, pptx, pdf, md, txt and common source-code files. Office and pdf text comes from the document text extraction of client/core-tools. Files larger than policy.knowledge.max_file_mb (default 25) MUST be skipped.

#### Scenario: Selected folder
- **WHEN** the user adds a permitted folder to the files Source
- **THEN** supported files in it are indexed, and later changes are picked up within 5 minutes

#### Scenario: Single document
- **WHEN** the user imports one permitted pdf document
- **THEN** that document is indexed, changes to it are picked up within 5 minutes, and no other file of its folder is indexed

#### Scenario: Oversized or unsupported file
- **WHEN** a selected folder contains a 200 MB pdf and a binary executable
- **THEN** both are skipped and are listed as skipped in the indexing status

## ADDED Requirements

### Requirement: Knowledge Notes
The client SHALL let the user write, edit and delete notes in the Knowledge view. Notes form the `notes` Source, are indexed like documents, and are subject to policy.knowledge.sources.notes (default on) under Source Enablement.

#### Scenario: Note found by the agent
- **WHEN** the user writes a note "Project Falcon starts on 1 March" and later asks the agent when Project Falcon starts
- **THEN** knowledge.search returns the note as a passage with the notes Source as provenance

### Requirement: Team Knowledge In Search
knowledge.search SHALL also query control-plane/team-knowledge for passages shared with the user's Groups, when the control plane is connected, and merge them with local results. Each team passage carries its owner, Group and item title as provenance and is delivered to the model as untrusted data, like any retrieved content. When the control plane is not connected, knowledge.search returns local results and says that team knowledge was not searched.

#### Scenario: Colleague's shared document
- **WHEN** bob is in engineering, alice has shared a document with engineering, and bob's agent calls knowledge.search with a matching query
- **THEN** the results include a passage of alice's document, labelled with alice as owner and engineering as Group
