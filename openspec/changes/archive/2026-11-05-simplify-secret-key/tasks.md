## 1. Implementation

- [x] 1.1 Derive the server key from a passphrase (scrypt), keep base64 32-byte keys as they are, and refuse values under 16 characters. Verify with connections.test.ts
- [x] 1.2 End startup on a settings error with one `refusing to start` line. Verify with a production-mode start
- [x] 1.3 README and `deploy/.env.example`: the passphrase, without openssl

## 2. Close

- [x] 2.1 Run `openspec validate --all --strict`
