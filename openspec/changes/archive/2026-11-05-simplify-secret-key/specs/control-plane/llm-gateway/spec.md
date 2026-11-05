# Spec Delta

## ADDED Requirements

### Requirement: Server Key From A Passphrase
The control plane SHALL derive the server key that encrypts LLM connection credentials from WORKBENCH_SECRET_KEY: a value that is the base64 encoding of 32 bytes is the key itself, and any other value of at least 16 characters is a passphrase from which the key is derived deterministically. A shorter value MUST stop the control plane at startup with a message naming WORKBENCH_SECRET_KEY, without a stack trace.

#### Scenario: Passphrase typed by an admin
- **WHEN** WORKBENCH_SECRET_KEY is set to a passphrase such as `correct horse battery staple` and an LLM connection is added
- **THEN** the control plane starts, the credential is stored encrypted, and after a restart with the same passphrase the connection still works

#### Scenario: Existing base64 key
- **WHEN** WORKBENCH_SECRET_KEY holds the base64 encoding of 32 bytes that a deployment already used
- **THEN** credentials stored under it are still decrypted

#### Scenario: Too short
- **WHEN** WORKBENCH_SECRET_KEY is `secret`
- **THEN** the control plane exits with `refusing to start: WORKBENCH_SECRET_KEY ...` and no stack trace
