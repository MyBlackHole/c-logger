"""Ordinary Logger crash-evidence contract and explicit Audit migration map."""

POINTS = (
    "acknowledged",
    "before_file_fsync",
    "after_file_fsync",
    "rotation_after_file_fsync",
    "file_after_archive_rename",
    "file_after_archive_dirsync",
    "file_after_active_open",
    "file_after_active_dirsync",
)

MIGRATION = {
    "previous_process_cases": 9,
    "previous_vm_points": 10,
    "current_process_cases": 8,
    "current_vm_points": 8,
    "retired_audit_only_points": {
        "before_state_rename": "Audit checkpoint transaction; no ordinary Logger equivalent",
        "after_state_rename": "Audit checkpoint transaction; no ordinary Logger equivalent",
        "after_checkpoint_commit": "Audit checkpoint transaction; no ordinary Logger equivalent",
    },
    "replacements": {
        "acknowledged": "ordinary Logger acknowledged; additionally covered by process SIGKILL",
        "before_audit_fsync": "before_file_fsync; additionally covered by process SIGKILL",
        "after_audit_fsync": "after_file_fsync",
        "audit_rotation_crash_regression/rotation_crash_sha256": "rotation_after_file_fsync; additionally covered by VM SIGKILL",
        "file_after_archive_rename": "same real backend hook, ordinary Logger byte verification",
        "file_after_archive_dirsync": "same real backend hook, ordinary Logger byte verification",
        "file_after_active_open": "same real backend hook, ordinary Logger byte verification",
        "file_after_active_dirsync": "same real backend hook, ordinary Logger byte verification",
    },
}
