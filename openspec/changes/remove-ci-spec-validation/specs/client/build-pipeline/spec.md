## MODIFIED Requirements

### Requirement: Remaining Pipeline Checks
The pipeline SHALL compile the client product, check manifests and footprint, optionally sign it, build the control-plane image, and publish both products. OpenSpec validation, parser fuzzing, control-plane and Graph mock tests SHALL remain available locally and SHALL NOT gate publication.

#### Scenario: Publish a main build
- **WHEN** product builds succeed for a main push
- **THEN** the workflow publishes the control-plane image and desktop package, with links to independently published optional packages

#### Scenario: An in-flight change fails strict validation
- **WHEN** a main push contains an OpenSpec change that fails `openspec validate --all --strict`
- **THEN** the pipeline does not run OpenSpec validation, and publication proceeds when the product builds succeed
