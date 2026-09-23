> Historical revision record, not the current crypto backend contract.
> The external backend has since been removed; see [BUILTIN_CRYPTO.md](BUILTIN_CRYPTO.md).
> Original test evidence and failure reports are preserved, not retroactively changed to pass.

# Round 3 — Crypto fail-closed and SM3 undefined behavior

This revision builds on the Round 2 Audit lifecycle/commit fixes. It does not
change the audit line format, the checkpoint v2 format, or the numeric values
of SHA-256/SM3. It is not a Production v1 release.

## Internal digest contract

`audit_digest_ops_t.hash()` returns **0 or negative errno**. Callers must check
it before using the output. Both compiled backends enforce these rules:

* A successful result is exactly 32 digest bytes.
* On any error, the caller's output buffer is unchanged. No error is encoded as
  a zero digest or a seemingly valid hexadecimal string.
* A NULL input with length 0 hashes the empty byte string. NULL with positive
  length and NULL output are EINVAL. Sizes above UINT64_MAX / 8 are rejected
  before reading input (SHA-256/SM3 have a 64-bit bit-length field).
* Registry presence does not establish runtime EVP availability. The internal
  `audit_digest_check()` hashes an empty message at initialization/recovery/
  verification entry, including paths with no records to replay.

Public Audit APIs retain 0 / -1 + errno. Error mapping:

| Failure | Error |
| --- | --- |
| Invalid arguments | EINVAL |
| Input length cannot be represented | EOVERFLOW |
| Unknown algorithm / invalid internal provider shape | EPROTONOSUPPORT |
| Selected EVP method unavailable at compile-time or getter returns NULL | ENOTSUP |
| EVP_MD_CTX_new returns NULL | ENOMEM |
| EVP init/update/final returns failure, or final length is not 32 | EIO |

`EIO` here can indicate a digest-engine error, not disk I/O; Audit state
separates CRYPTO_FAILED from IO_FAILED. OpenSSL's error queue is not cleared or
drained by this helper. The caller may inspect its thread-local OpenSSL errors.
The helper returns the first local failure even if cleanup changes errno.

## EVP behavior

The EVP context is local to each hash call and is freed on every post-allocation
exit. Init failure does not call Update/Final. Update failure does not call
Final. Final writes only to private temporary storage; publication occurs only
after success and an exact-length check.

OpenSSL 1.1.1-compatible EVP APIs are retained; OpenSSL >= 1.1.1 is required by
CMake for that build option. Only the selected implementation is compiled:
there is **no fallback from EVP to builtin**, no implicit switch of algorithm,
and no implicit switch to AUDIT_INTEGRITY_NONE. The latter remains an explicit
configuration for a destination that intentionally has no hash chain.

Runtime policy failure was tested with an actual OpenSSL 3 default-property
query naming a nonexistent provider, not only with mocked return values. Tests
change this policy sequentially in dedicated processes. Applications must not
copy that fault-test procedure as a concurrent runtime reconfiguration API:
EVP_set_default_properties is not thread-safe and is intended for libctx setup.

Reference: OpenSSL manual EVP_DigestInit (Return Values / Notes),
EVP_set_default_properties (Notes). Both were checked during this revision.

## Audit state

`AUDIT_STATE_CRYPTO_FAILED` is appended to the public enum; previous enum values
and status-struct layout are unchanged.

For a detected digest failure while constructing a record:

1. No logger write is issued for this record. No sequence, chain head, or
   checkpoint for it is advanced. A failing audit_begin may still have reserved
   a transaction ID; that is not a committed record or permission to run work.
2. The runtime latches CRYPTO_FAILED and its error.
3. New write/begin/end calls fail; audit_flush does not clear this state.
4. audit_shutdown_status releases resources, reports the error, and does not
   append a normal AUDIT_STOP. The void shutdown is still a compatibility wrapper.
5. After correcting the engine/policy problem, explicitly shut down/reinitialize.
   Reinitialization remains subject to the still-documented recovery limits.

Input/encoding-size errors remain ordinary pre-output errors and do not latch
CRYPTO_FAILED. IO_FAILED continues to mean the write/sync outcome may be
uncertain; the new crypto behavior does not weaken Round 2's handling of it.
AUDIT_FAIL_REPORT versus AUDIT_FAIL_DENY is still a business-callsite policy;
the library cannot automatically roll back the caller's protected operation.

If preflight fails during init, no lock/log/checkpoint file is created and no
recovery modification is attempted. If the engine fails later during START,
the candidate is not published and no fake STOP is written. At that later point
initialization may already have created an empty log/derived checkpoint.

## Recovery and verification

Every actual hash return is checked. Recovery carries a private candidate
checkpoint and publishes it to the caller only after successful processing.
Crypto failure preserves errno through stream/resource cleanup and does not
advance that result. Preflight failure does not trigger EOF-tail repair.
Verification returns engine errors rather than treating them as a computed
hash; the optional final-hash buffer is unchanged on failure. In particular,
an old all-zero forged record does not verify when EVP is unavailable.

This does NOT finish the parser/recovery repair. Quoted marker parsing, strict
line suffixes, archive identity, long-line/partial-EOF distinction, historical
checkpoint verification and cross-archive ordering remain release blockers.
A current checkpoint can still bypass validation of historical bytes. Previously
written zero-digest/broken history must be preserved and investigated, not
silently rehashed or declared repaired by the new implementation.

## Builtin SM3

Rotation count 0 (also 32 after modulo reduction) now returns the original
32-bit word. The undefined right shift by 32 is removed. Shared one-shot padding
handles empty input without memcpy(NULL, 0), both padding-block cases, and big-
endian bit length. Builtin SHA-256/SM3 are not compiled in an EVP build.

Known-answer tests include empty input, abc, standard multi-block inputs, one
million 'a' bytes, plus 148 deterministic binary lengths per algorithm. Binary
reference fixtures are checked in and regenerate through Python hashlib, not
through the implementation under test. OpenSSL's official EVP SHA/SM3 test data
was used to cross-check the abc and standard multi-block expected values.
Eight-thread per-operation hashing and both directions of cross-backend chain
continuation are also tested. These checks are not a cryptographic certification.

## Validation limitations

Release suites and the scoped crypto sanitizer suites passed. Full
ASan/UBSan validation is **not stable/green**: the existing ordinary Logger
fork+continue test timed out with both backends (builtin passed once, then
failed on the next full run; OpenSSL failed its full run). This also reproduced against the unmodified Round
2 input (after one successful isolated run). Root cause is not attributed to
CPU throttling or to a specific library here. It remains tracked with the
ordinary Logger fork contract; no test was removed or disabled to hide it.
