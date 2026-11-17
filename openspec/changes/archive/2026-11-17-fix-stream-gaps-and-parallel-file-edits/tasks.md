# Tasks

## 1. Gateway

- [x] 1.1 Keep-alive comments only at event boundaries (D1). Test against a fake upstream that stalls in the middle of an event for longer than the keep-alive interval: the client receives the event intact.

## 2. Client

- [x] 2.1 A streamed data payload that does not parse fails the response as malformed (D2). Unit test.
- [x] 2.2 The per-file lock in files.read, files.write and files.edit (D3). Unit test: parallel edits on one file all apply.
