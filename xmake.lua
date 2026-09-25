set_project("prod_c_logger")
set_xmakever("2.8.5")

-- Xmake is a parity build at this stage. CMake remains authoritative for
-- install/package/release until the later migration gates are completed.
option("build_shared")
    set_default(false)
    set_showmenu(true)
    set_description("Build the production logger as a shared library")
option_end()

option("legacy_fork")
    set_default(false)
    set_showmenu(true)
    set_description("Build the compatibility-only logger_fork_reinit helper")
option_end()

option("build_tests")
    set_default(false)
    set_showmenu(true)
    set_description("Build and register the production-linked core test set")
option_end()

set_allowedplats("linux")

local project_version = "0.9.3"
local abi_version = "0"

local logger_sources = {
    "src/logger.c",
    "src/logger_global.c",
    "src/logger_process.c",
    "src/logger_scope.c",
    "src/logger_redact.c",
    "src/logger_format.c",
    "src/logger_queue.c",
    "src/logger_file.c",
    "src/logger_file_rename.c",
    "src/logger_syslog.c",
    "src/logger_worker.c",
    "src/audit.c",
    "src/audit_record.c",
    "src/audit_integrity.c",
    "src/audit_recovery.c",
    "src/audit_verify.c",
    "src/console.c"
}

target("logger")
    set_kind(has_config("build_shared") and "shared" or "static")
    if has_config("build_shared") then
        set_version(project_version, {soname = abi_version})
    end

    for _, source in ipairs(logger_sources) do
        add_files(source)
    end
    if has_config("legacy_fork") then
        add_files("src/logger_fork.c")
        add_defines("LOGGER_ENABLE_LEGACY_FORK_HELPER=1", {public = true})
    end

    -- Match the production dialect/visibility contract rather than inheriting
    -- Xmake's built-in release rule (which strips by default).
    add_cflags("-std=gnu11", "-fPIC", "-Wall", "-Wextra", "-Wpedantic", "-Werror", {force = true})
    set_symbols("hidden")
    if is_mode("release") then
        set_optimize("fastest")
        add_defines("NDEBUG")
    elseif is_mode("debug") then
        set_optimize("none")
        set_symbols("debug", "hidden")
    end

    add_defines("LOGGER_ENABLE_FAULT_INJECTION=0")
    if not has_config("build_shared") then
        add_defines("LOGGER_STATIC_DEFINE=1", {public = true})
    end
    add_syslinks("pthread", {public = true})

    set_configdir("$(builddir)/generated")
    add_configfiles("include/logger_version.h.in", {
        filename = "logger_version.h",
        pattern = "@(.-)@",
        variables = {
            PROJECT_VERSION_MAJOR = "0",
            PROJECT_VERSION_MINOR = "9",
            PROJECT_VERSION_PATCH = "3",
            PROJECT_VERSION = project_version,
            LOGGER_ABI_VERSION = abi_version
        }
    })
    add_includedirs("include", "$(builddir)/generated", {public = true})

    on_load(function (target)
        if target:kind() == "shared" then
            local manifest = path.join(os.projectdir(), "cmake", "logger.symbols")
            local out = {"LOGGER_0.9 {\n", "  global:\n"}
            for line in io.lines(manifest) do
                local name = line:match("^%s*(.-)%s*$")
                if name ~= "" and name:sub(1, 1) ~= "#" then
                    assert(name:match("^[a-z][a-z0-9_]+$"),
                           "invalid ABI symbol in " .. manifest .. ": " .. name)
                    table.insert(out, "    " .. name .. ";\n")
                end
            end
            if has_config("legacy_fork") then
                table.insert(out, "    logger_fork_reinit;\n")
            end
            table.insert(out, "  local: *;\n};\n")

            local mapfile = path.join(target:autogendir(), "logger.map")
            os.mkdir(path.directory(mapfile))
            io.writefile(mapfile, table.concat(out))
            target:add("shflags", "-Wl,--version-script=" .. mapfile, {force = true})
            target:add("shflags", "-Wl,--no-undefined", {force = true})
            target:add("shflags", "-Wl,--no-undefined-version", {force = true})
        end
    end)
target_end()


if has_config("build_tests") then
    -- Phase 2A: only tests that link the production logger directly. White-box
    -- regression/fault/crash targets remain on CMake until their private support
    -- libraries and --wrap contracts are migrated explicitly.
    local logger_tests = {
        {"test_logger", "tests/test_logger.c", 60},
        {"rotation_test", "tests/test_rotation.c", 60},
        {"audit_test", "tests/test_audit.c", 60},
        {"audit_failure_test", "tests/test_audit_failure.c", 60},
        {"audit_transaction_test", "tests/test_audit_transaction.c", 60},
        {"console_test", "tests/test_console.c", 60},
        {"format_test", "tests/test_format.c", 60},
        {"context_test", "tests/test_context.c", 60},
        {"permissions_test", "tests/test_permissions.c", 60},
        {"audit_instance_test", "tests/test_audit_instance.c", 60},
        {"audit_integrity_test", "tests/test_audit_integrity.c", 60},
        {"audit_restart_test", "tests/test_audit_restart.c", 60},
        {"audit_recovery_test", "tests/test_audit_recovery.c", 60},
        {"audit_rotation_recovery_test", "tests/test_audit_rotation_recovery.c", 60},
        {"fork_test", "tests/test_fork.c", 60},
        {"audit_fork_test", "tests/test_audit_fork.c", 60},
        {"fork_guard_test", "tests/test_fork_guard.c", 5},
        {"lifecycle_test", "tests/test_lifecycle.c", 10},
        {"concurrency_stress_test", "tests/test_concurrency_stress.c", 20},
        {"api_contract_test", "tests/test_api_contract.c", 60},
        {"config_abi_test", "tests/test_config_abi.c", 60},
        {"audit_single_writer_test", "tests/test_audit_single_writer.c", 60},
        {"redaction_test", "tests/test_redaction.c", 60},
        {"reinit_test", "tests/test_reinit.c", 10},
        {"multi_instance_test", "tests/test_multi_instance.c", 60}
    }

    for _, spec in ipairs(logger_tests) do
        target(spec[1])
            set_kind("binary")
            set_default(false)
            add_files(spec[2])
            add_deps("logger")
            add_cflags("-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", {force = true})
            add_tests("default", {timeout = spec[3]})
        target_end()
    end
end
