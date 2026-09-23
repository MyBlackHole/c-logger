# Logging and audit data security

## Secret policy

The logging library does not attempt to parse arbitrary printf text and guess
which substrings are passwords or keys. Such heuristics are incomplete and can
create a false security boundary.

Callers must not log plaintext passwords, encryption keys, private keys, access
key secrets, bearer tokens, session tokens, recovery codes, or equivalent
authentication material.

For values that must be referenced, prefer a stable non-secret identifier. If a
value must be partially recognizable, use `logger_mask_secret()`. To suppress a
value entirely, use `logger_redact()` or `AUDIT_DETAIL_REDACTED`.

## Audit fields

`actor`, `source`, `resource`, `operation`, and `detail` are audit metadata, not
secret containers. Key IDs, user IDs, object IDs, operation names, result codes
and similar identifiers are appropriate. Key material and authentication
secrets are not.

## Limitations

Redaction helpers only protect values passed through them. They cannot prevent a
caller from directly writing a secret with `LOG_INFO("%s", secret)`. Preventing
that requires application policy, code review, static analysis, and tests at the
call sites.

## Crypto engine failures

Round 3 makes digest errors explicit and refuses further Audit writes after a
runtime CRYPTO_FAILED state. Engine failures are not all-zero digests. A crypto failure does not silently reopen the old session; investigate,
then shut down/reinitialize explicitly. This is not FIPS/GM/T certification or malicious-provider validation.
An unkeyed chain is still not authenticated tamper prevention. Previously written
zero-digest or otherwise suspect files require preservation and investigation,
not automatic rehashing. See docs/ROUND3_CRYPTO.md and docs/KNOWN_ISSUES.md.

## Builtin-only implementation

Only the self-contained SHA-256 implementation is compiled. No
external provider policy, external crypto configuration, or dynamic crypto
engine is used. This intentionally gives up the option of routing digests
through an externally certified module. Algorithm correctness tests are not
FIPS, GM/T or cryptographic-module certification. Users that require such a
module must not assume this build satisfies that requirement.

The SHA-256 fail-closed error path remains. Retired algorithm ID 2 is rejected;
old SM3 history is not automatically rewritten or accepted as SHA-256. Removing an
unused external-library path is not a diagnosis or fix of the earlier external
library TSan reports; those observations are preserved as historical evidence.
