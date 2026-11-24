## 1. Lock only on the control plane's revocation (client/control-channel)

- [x] 1.1 Write unit tests in `client/tests/channel/control_channel_test.cpp`: `locks_installation` is true for `DENIED`/`denied`/`revoked` and false for `http_403`, other HTTP codes, `FORBIDDEN`, `INSTALLATION_CONFLICT` and an empty code. Verify: `control_channel_tests` passes.
- [x] 1.2 In `client/src/channel/control_channel.cpp`, lock in registration and heartbeat only when `locks_installation` holds; after a push-handshake 403, re-register and lock only through that registration. Verify: tests 1.1 pass and the client builds.
- [ ] 1.3 Acceptance, on a laptop whose network the control-plane front end refuses (empty-body 403): start the client. Verify: the log shows "the network refused the connection to the control plane", no "installation locked", and the client connects without a restart once it is on an allowed network.
