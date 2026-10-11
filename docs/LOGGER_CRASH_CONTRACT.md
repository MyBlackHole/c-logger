# Ordinary Logger crash and VM evidence contract

This is the current main-line contract. It replaces Audit-backed crash acceptance;
no Audit transaction, checkpoint, hash chain, or automatic torn-tail repair is
claimed. Historical release notes describe the old product and are not rewritten.

## What is acknowledged

A fresh, synchronous, file-only `logger_t` writes ten different records with sequence
numbers 000–009. Each `logger_log_sync_status()` must return success, followed by a
successful `logger_flush_instance_status()`. Only then is any cut point armed.
The process stays open: successful destruction is not used to manufacture the
baseline's persistence. A test-only fixed realtime clock and UTC timezone make the
whole formatted record deterministic, including timestamp, level, sequence,
payload, terminator, and newline. Monotonic clocks and deadlines remain real.

Rotation uses a one-byte size threshold so each nonempty archive contains exactly
one complete record. Retention is disabled. Nine archives exist before an armed
rotation; the next write must perform the real tenth rotation. Strict archive
names are sorted, active is read last, and the complete byte stream must match
all ten distinct baseline records in order, exactly once. Wrong payloads, duplicate
or missing records, partial archives, unexpected names in the `crash.*` namespace,
symlinks, hard links, extra records, and oversized files fail verification.

Recovery first verifies the untouched files. A new ordinary Logger then appends
sequence 011 and closes; another new instance appends 012 and closes. Each write,
flush, and destroy must succeed. Each open also checks that a competing owner gets
`EBUSY`; successful subsequent opens prove the killed/prior owner released its
lease. The complete inventory is checked after each append, including all baseline
and recovery bytes. These are test-oracle exactly-once checks of a serial writer,
not an application-level transactional exactly-once delivery guarantee.

## Eight cut points, applied to both harnesses

| Point | Actual stop boundary | Interrupted sequence 010 |
| --- | --- | --- |
| `acknowledged` | After all ten baseline write acknowledgments and explicit flush | Absent: never submitted |
| `before_file_fsync` | Immediately before the target data fd's real `fsync()` | Absent, complete, or an exact byte prefix of the expected target |
| `after_file_fsync` | Immediately after that real data `fsync()` returns success | Complete, exactly once; API call has not returned |
| `rotation_after_file_fsync` | After a completed real rotation and target data `fsync()` success | Complete, exactly once, after ten distinct archives |
| `file_after_archive_rename` | Existing backend hook after `RENAME_NOREPLACE` | Absent: rotation precedes target write; rename may persist or roll back |
| `file_after_archive_dirsync` | Existing hook after archive directory sync | Absent; ten archives must survive |
| `file_after_active_open` | Existing hook after creating new active, before its inode/directory sync | Absent; ten archives survive, empty active may be absent |
| `file_after_active_dirsync` | Existing hook after new active inode and directory sync | Absent; ten archives and the empty active must survive |

Only test binaries use `--wrap=fsync`, `--wrap=clock_gettime`, and
`--wrap=logger_fault_crash_if_requested`. The fsync wrapper compares the actual fd's
regular-file identity to the active inode and requires the exact expected postwrite
size. It cannot stop on a directory fsync, an archived fd, or the empty-new-inode
fsync inside rotation. The rotation-plus-fsync case additionally requires the real
`file_after_active_dirsync` hook first. Production sources and installed libraries
receive no new hooks. The unused legacy internal `before_audit_fsync` hook has
been removed; these eight cut boundaries are unchanged.

The unconfirmed pre-fsync target may be lost or cut short. Only a byte prefix of
its exact expected content is accepted, never arbitrary sector corruption or
unrelated garbage. Its tail need not end in a newline: verification uses byte
lengths and full recovery suffixes, not line splitting. Ordinary Logger does not
repair that torn tail; the next formatted record may be adjacent to it. This test
requires the new records' complete bytes to be appended, not Audit-style record
salvage. Other physical corruption models require separate qualification.

## Matrix migration, not silent coverage reduction

| Old evidence | New evidence or explicit retirement |
| --- | --- |
| VM `acknowledged` | Ordinary `acknowledged`, now also process SIGKILL |
| VM `before_audit_fsync` | Actual ordinary `before_file_fsync`, now also process SIGKILL |
| Process/VM `after_audit_fsync` | Actual ordinary `after_file_fsync` |
| Process `rotation_crash_sha256` | Ordinary `rotation_after_file_fsync`, now also VM |
| Four `file_crash_sha256_file_after_*` / VM rotation points | Same four actual file backend hooks; complete ordinary Logger byte oracle |
| `before_state_rename` | Retired: Audit checkpoint transaction has no ordinary Logger equivalent |
| `after_state_rename` | Retired: Audit checkpoint transaction has no ordinary Logger equivalent |
| `after_checkpoint_commit` | Retired: Audit checkpoint transaction has no ordinary Logger equivalent |

The previous process set was nine: four Audit fsync/checkpoint cases, one Audit
rotation case, and four file rotation cases. Removing three checkpoint-only cases
and adding acknowledged/pre-fsync process checks yields eight. The previous VM
set was ten: removing those three checkpoint-only points and adding the rotated
post-fsync point yields eight. `scripts/crash_contract.py` records this mapping in
both JSON evidence formats. Historical Audit rotation and busy/cwd tests have
been retired separately; the ordinary `process-crash` group is unchanged.

## Distinct failure boundaries and evidence

- Process: a fresh exec writer reports the exact reached point through a pipe;
  only then does the controller send `SIGKILL`. Wrong/missing markers or a ten-second
  timeout fail, even if cleanup kills the writer. The controller requires actual
  `WIFSIGNALED` / `SIGKILL` status. The kernel and its page cache remain alive.
- VM: the guest prints the exact point to its serial console and waits without
  cleanup or guest sync. The host requires that line, SIGKILLs QEMU, verifies the
  signal return code, and reboots with the same raw disk (`cache=none`). The recovery
  boot must print its success marker, exit the guest program with zero, and shut
  QEMU down successfully. Guest page cache is lost at the kill.
- VM CI runs the full eight points on both raw ext4 and XFS, including PRs. Serial
  logs record guest kernel/filesystem and each inventory result; `vm-matrix.json`
  records the selected point, migration, phase markers, and QEMU outcomes.
- Process CI retains the existing shared/static build-configuration matrix and
  repetition policy (PR: shared once; main/manual: both configurations, three times).
  Both configurations deliberately execute a private static support archive, not
  fault injection through the production shared DSO. Exact case-set checking emits
  JSON and JUnit; no missing case can turn green.

Local execution on OverlayFS tests process recovery and the oracle; it does not
establish ext4/XFS VM persistence. QEMU/raw-disk evidence in turn is not a physical
PDU cut, a storage-controller volatile-cache test, or qualification of arbitrary
server/NVMe/SATA/RAID/HBA/mount combinations. Required target-storage acceptance
remains separate.

## Commands

```sh
export LOGGER_PROJECT_VERSION="$(cat VERSION)"
xmake f -m release -o build --build_shared=n --build_private_tests=y
xmake test -g process-crash -j1 -vD | tee crash.log
python3 scripts/check_crash_matrix.py crash.log --output crash-matrix.json --junit-dir .
xmake test -g crash-verifier -j1 -vD
xmake vm_powercut_guest
```

The independent oracle-mutation test covers every allowed pre-fsync byte-prefix
length with and without recovery suffixes, plus missing/duplicate/mutated baseline,
recovery, archived content, and hostile namespace fixtures. It also checks Xmake
and the full VM matrix against the declared point set. It is an oracle check,
not a substitute for actually killing a process or VM.
