# LOGGER_1.0 Semantic ABI Contract

This document accompanies `abi/logger-1.0.symbols` and
`tests/packaging/v1_abi_contract.c`.

The executable contract is authoritative for the initial Production v1 public
surface on Linux/ELF x86_64.

## Frozen compatibility surface

For every public type that exists at the 1.0 freeze point, the following are
stable across compatible 1.x releases:

- aggregate size and alignment;
- offsets of existing public fields;
- numeric values of existing enum constants and public numeric macros;
- public function names and exact C function types;
- configuration version numbers;
- existing default configuration values and meanings.

The initial frozen dynamic symbol surface contains 66 default production
exports and is listed in `abi/logger-1.0.symbols`.

## Additive 1.x changes

A later 1.x release may add a new API only when it does not invalidate the
existing contract. Examples include:

- a new function with a new symbol;
- a new opaque type;
- a new standalone metrics/config structure;
- an appended enum value when no existing numeric value changes.

An additive public symbol requires explicit ABI review and corresponding
updates to the active allowlist and v1 snapshot policy. It must not silently
reuse a retired numeric ID.

## Incompatible changes

The following require a new ABI major rather than an ordinary 1.x update:

- removing or changing the type of an existing public function;
- changing an existing struct field type, offset, size or alignment;
- inserting fields into an existing frozen structure in a way that changes the
  layout seen by existing binaries;
- renumbering an existing enum or public numeric constant;
- changing ownership/lifetime or error conventions in a way that makes a
  previously valid binary caller unsafe;
- changing the meaning of an existing config version without an explicit
  compatibility mechanism.

## Semantic rules not encoded by ELF metadata

The binary checker cannot express ownership and lifecycle semantics. The
following remain part of the v1 contract and are documented in the public
headers/API docs:

- host-owned explicit `logger_t` is the primary ownership model;
- callers stop/join borrowers before destroy/unload;
- status APIs use the documented 0/-1+errno convention except the explicitly
  retained `logger_log_sync_status()` negative-errno convention;
- ordinary Logger/Console entry is not signal-safe or hard-real-time;
- Audit remains process-global single-writer state;
- local Syslog success is not durable remote acknowledgement;
- the local SHA-256 Audit chain is integrity evidence, not a trusted external
  anti-rollback/WORM mechanism.

Any change to these semantics requires the same compatibility review as a
binary ABI change, even when symbol names and layouts remain identical.

## Enforcement

`v1_abi_contract.c` is compiled and run as both C11 and C++11. It freezes:

- all current public aggregate layouts;
- public enum/macro numeric values;
- all 66 default-production function types;
- Logger/Audit/Console default configuration values.

`release-validation` runs this contract for both shared and static release
profiles. `xmake-parity` also has a dedicated semantic-ABI job.

The separate ELF gate continues to verify SONAME, symbol version, visibility and
the public symbol allowlist.
