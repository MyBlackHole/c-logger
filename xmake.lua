set_project("prod_c_logger")
set_xmakever("2.8.5")
includes("@builtin/xpack")

-- Xmake is the sole project build/test/install/package authority.
-- Installed CMake/pkg-config metadata remains a downstream compatibility surface.
option("build_shared")
    set_default(false)
    set_showmenu(true)
    set_description("Build the production logger as a shared library")
option_end()

option("v1_abi_preview")
    set_default(false)
    set_showmenu(true)
    set_description("Build the non-default Production v1 ABI preview (SONAME 1 / LOGGER_1.0)")
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

option("build_private_tests")
    set_default(false)
    set_showmenu(true)
    set_description("Build and register the test-only support cases")
option_end()

option("build_regression_tests")
    set_default(false)
    set_showmenu(true)
    set_description("Build and register the regression-support cases")
option_end()

-- Xmake description scope deliberately disallows file I/O. VERSION remains the
-- single source; callers export it once and every Xmake subprocess inherits it.
set_allowedplats("linux")

local project_version = os.getenv("LOGGER_PROJECT_VERSION") or "0.0.0"
local version_major, version_minor, version_patch =
    project_version:match("^(%d+)%.(%d+)%.(%d+)$")
local v1_abi_preview = has_config("v1_abi_preview")
local abi_version = v1_abi_preview and "1" or "0"
local symbol_version = v1_abi_preview and "LOGGER_1.0" or "LOGGER_0.9"
local abi_manifest = v1_abi_preview and "abi/logger-1.0.symbols" or "abi/logger.symbols"

local function cmake_bool(value)
    return value and "TRUE" or "FALSE"
end

local function configure_install_metadata(target, mkdir, writefile)
    local shared = target:kind() == "shared"
    local legacy = has_config("legacy_fork")
    local generated = path.join(target:autogendir(), "install")
    mkdir(generated)

    local config = string.format([[
include(CMakeFindDependencyMacro)
find_dependency(Threads)
set(Logger_FOUND TRUE)
set(Logger_VERSION "%s")
set(Logger_ABI_VERSION "%s")
set(Logger_CRYPTO_IMPLEMENTATION "builtin-sha256")
set(Logger_RELEASE_CANDIDATE TRUE)
set(Logger_shared_FOUND %s)
set(Logger_static_FOUND %s)
set(Logger_legacy_fork_FOUND %s)
include("${CMAKE_CURRENT_LIST_DIR}/LoggerTargets.cmake")
foreach(_comp IN LISTS Logger_FIND_COMPONENTS)
  if((NOT DEFINED Logger_${_comp}_FOUND OR NOT Logger_${_comp}_FOUND)
     AND Logger_FIND_REQUIRED_${_comp})
    set(Logger_FOUND FALSE)
  endif()
endforeach()
]], project_version, abi_version, cmake_bool(shared), cmake_bool(not shared),
       cmake_bool(legacy))
    local config_file = path.join(generated, "LoggerConfig.cmake")
    writefile(config_file, config)

    local compile_definitions = {}
    if not shared then
        table.insert(compile_definitions, "LOGGER_STATIC_DEFINE=1")
    end
    if legacy then
        table.insert(compile_definitions, "LOGGER_ENABLE_LEGACY_FORK_HELPER=1")
    end
    local definitions = table.concat(compile_definitions, ";")
    local library_type = shared and "SHARED" or "STATIC"
    local library_file = shared and ("liblogger.so." .. project_version) or "liblogger.a"
    local soname = shared and '  IMPORTED_SONAME "liblogger.so.' .. abi_version .. '"\n' or ""
    local defprop = definitions ~= "" and
        ('  INTERFACE_COMPILE_DEFINITIONS "' .. definitions .. '"\n') or ""
    local targets = string.format([[
if(TARGET Logger::logger)
  return()
endif()
get_filename_component(_IMPORT_PREFIX "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
add_library(Logger::logger %s IMPORTED)
set_target_properties(Logger::logger PROPERTIES
  IMPORTED_LOCATION "${_IMPORT_PREFIX}/lib/%s"
%s  INTERFACE_INCLUDE_DIRECTORIES "${_IMPORT_PREFIX}/include/logger"
  INTERFACE_LINK_LIBRARIES "Threads::Threads"
%s  INTERFACE_LOGGER_ABI "%s"
  INTERFACE_LOGGER_VARIANT "production"
)
set_property(TARGET Logger::logger APPEND PROPERTY COMPATIBLE_INTERFACE_STRING
  LOGGER_ABI LOGGER_VARIANT)
unset(_IMPORT_PREFIX)
]], library_type, library_file, soname, defprop, abi_version)
    local targets_file = path.join(generated, "LoggerTargets.cmake")
    writefile(targets_file, targets)

    -- Match CMake's installed export layout: keep the general imported
    -- properties above for config-agnostic consumers, and also provide the
    -- RELEASE configuration fragment that CMake install(EXPORT) emits.
    local release_location = shared and ("liblogger.so." .. project_version) or "liblogger.a"
    local release_soname = shared and
        ('  IMPORTED_SONAME_RELEASE "liblogger.so.' .. abi_version .. '"\n') or ""
    local release_targets = string.format([[
set_property(TARGET Logger::logger APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(Logger::logger PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/%s"
%s)
]], release_location, release_soname)
    local targets_release_file = path.join(generated, "LoggerTargets-release.cmake")
    writefile(targets_release_file, release_targets)

    local version = string.format([[
set(PACKAGE_VERSION "%s")
if(PACKAGE_FIND_VERSION STREQUAL PACKAGE_VERSION)
  set(PACKAGE_VERSION_EXACT TRUE)
  set(PACKAGE_VERSION_COMPATIBLE TRUE)
else()
  set(PACKAGE_VERSION_COMPATIBLE FALSE)
endif()
]], project_version)
    local version_file = path.join(generated, "LoggerConfigVersion.cmake")
    writefile(version_file, version)

    local pc_definitions = ""
    if not shared then
        pc_definitions = pc_definitions .. " -DLOGGER_STATIC_DEFINE=1"
    end
    if legacy then
        pc_definitions = pc_definitions .. " -DLOGGER_ENABLE_LEGACY_FORK_HELPER=1"
    end
    local pc = string.format([[prefix=${pcfiledir}/../..
exec_prefix=${prefix}
libdir=${prefix}/lib
includedir=${prefix}/include/logger

Name: prod-c-logger
Description: Host-owned Logger and Audit (builtin SHA-256), controlled-production candidate
Version: %s
Libs: -L${libdir} -llogger
Libs.private: -pthread
Cflags: -I${includedir}%s
]], project_version, pc_definitions)
    local pc_file = path.join(generated, "logger.pc")
    writefile(pc_file, pc)

    target:add("installfiles", config_file, targets_file, targets_release_file,
               version_file, {prefixdir = "lib/cmake/Logger"})
    target:add("installfiles", pc_file, {prefixdir = "lib/pkgconfig"})
end

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
            PROJECT_VERSION_MAJOR = version_major,
            PROJECT_VERSION_MINOR = version_minor,
            PROJECT_VERSION_PATCH = version_patch,
            PROJECT_VERSION = project_version,
            LOGGER_ABI_VERSION = abi_version
        }
    })
    add_includedirs("include", "$(builddir)/generated", {public = true})
    add_headerfiles("include/logger.h", "include/audit.h", "include/console.h",
                    "include/logger_export.h", {prefixdir = "logger"})
    add_installfiles("$(builddir)/generated/logger_version.h",
                     {prefixdir = "include/logger"})
    if has_config("legacy_fork") then
        add_headerfiles("include/logger_fork_compat.h", {prefixdir = "logger"})
    end

    on_load(function (target)
        if project_version == "0.0.0" or not version_major then
            raise("export LOGGER_PROJECT_VERSION=$(cat VERSION) before invoking Xmake")
        end
        configure_install_metadata(target, os.mkdir, io.writefile)

        target:add("installfiles", "LICENSE", "API.md", "SECURITY.md", "TESTING.md", "CHANGELOG.md",
                   {prefixdir = "share/doc/prod_c_logger"})
        for _, doc in ipairs(os.files(path.join(os.projectdir(), "docs", "*.md"))) do
            if path.filename(doc):sub(1, 7) ~= "HISTORY" then
                target:add("installfiles", doc,
                           {prefixdir = "share/doc/prod_c_logger/docs"})
            end
        end
        target:add("installfiles", "examples/installed_consumer/CMakeLists.txt",
                   "examples/installed_consumer/main.c",
                   "examples/installed_consumer/cpp.cpp",
                   "examples/installed_consumer/plugin.c",
                   "examples/installed_consumer/plugin_loader.c",
                   {prefixdir = "share/doc/prod_c_logger/examples/installed_consumer"})

        if target:kind() == "shared" then
            local manifest = path.join(os.projectdir(), abi_manifest)
            local out = {symbol_version .. " {\n", "  global:\n"}
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

-- Non-default auxiliary tools/examples.
target("bench_logger")
    set_kind("binary")
    set_default(false)
    add_files("tests/bench_logger.c")
    add_deps("logger")
    add_cflags("-std=gnu11", {force = true})
target_end()

target("v1_abi_contract_c")
    set_kind("binary")
    set_default(false)
    add_files("tests/packaging/v1_abi_contract.c")
    add_deps("logger")
    add_cflags("-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", {force = true})
target_end()

target("v1_abi_contract_cpp")
    set_kind("binary")
    set_default(false)
    add_files("tests/packaging/v1_abi_contract.cpp")
    add_deps("logger")
    add_cxxflags("-std=c++11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", {force = true})
target_end()

target("real_syslogd_integration")
    set_kind("binary")
    set_default(false)
    add_files("tests/integration/test_real_syslogd.c")
    add_deps("logger")
    add_cflags("-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", {force = true})
target_end()

target("multi_instance_example")
    set_kind("binary")
    set_default(false)
    add_files("examples/multi_instance.c")
    add_deps("logger")
    add_cflags("-std=gnu11", {force = true})
target_end()

target("syslog_client_example")
    set_kind("binary")
    set_default(false)
    add_files("examples/syslog_client.c")
    add_deps("logger")
    add_cflags("-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
               {force = true})
target_end()

if has_config("legacy_fork") then
    target("fork_reinit_example")
        set_kind("binary")
        set_default(false)
        add_files("examples/fork_reinit.c")
        add_deps("logger")
        add_cflags("-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                   {force = true})
    target_end()
end

local package_kind = has_config("build_shared") and "shared" or "static"
local package_basename = v1_abi_preview
    and ("prod-c-logger-v1-abi-preview-" .. project_version .. "-Linux-x86_64-" .. package_kind)
    or ("prod-c-logger-" .. project_version .. "-Linux-x86_64-" .. package_kind)
local package_title = v1_abi_preview
    and ("c-logger " .. project_version .. " Production v1 ABI preview")
    or ("c-logger " .. project_version .. " controlled production candidate")
xpack("logger_package")
    set_formats("targz")
    set_version(project_version)
    set_title(package_title)
    set_description("Host-owned C Logger and Audit, builtin SHA-256")
    set_basename(package_basename)
    add_targets("logger")
    after_package(function (package)
        local output = package:outputfile()
        local digest = hash.sha256(output)
        io.writefile(output .. ".sha256",
                     digest .. "  " .. path.filename(output) .. "\n")
    end)
xpack_end()


if has_config("build_tests") then
    -- Production-linked core tests.
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


if has_config("build_private_tests") or has_config("build_regression_tests") then
    -- This archive intentionally carries the environment-driven test hooks.
    -- It is never installed and is never linked into the production logger.
    target("logger_test_support")
        set_kind("static")
        set_default(false)
        for _, source in ipairs(logger_sources) do
            add_files(source)
        end
        if has_config("legacy_fork") then
            add_files("src/logger_fork.c")
            add_defines("LOGGER_ENABLE_LEGACY_FORK_HELPER=1", {public = true})
        end
        add_files("src/logger_fault.c")
        add_cflags("-std=gnu11", "-fPIC", "-Wall", "-Wextra", "-Wpedantic", "-Werror", {force = true})
        set_symbols("hidden")
        add_defines("LOGGER_ENABLE_FAULT_INJECTION=1")
        add_defines("LOGGER_STATIC_DEFINE=1", {public = true})
        add_syslinks("pthread", {public = true})
        add_includedirs("include", "$(builddir)/generated", {public = true})
    target_end()
end

if has_config("build_private_tests") then
    target("fault_test")
        set_kind("binary")
        set_default(false)
        add_files("tests/test_faults.c")
        add_deps("logger_test_support")
        add_cflags("-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", {force = true})
        add_tests("file_write", {
            runargs = "logger",
            runenvs = {LOGGER_FAULT_POINT = "file_write", LOGGER_FAULT_ERRNO = "5"},
            timeout = 10
        })
        add_tests("file_fsync", {
            runargs = "logger",
            runenvs = {LOGGER_FAULT_POINT = "file_fsync", LOGGER_FAULT_ERRNO = "5"},
            timeout = 10
        })
        add_tests("state_write", {
            runargs = "audit",
            runenvs = {LOGGER_FAULT_POINT = "state_write", LOGGER_FAULT_ERRNO = "5"},
            timeout = 10
        })
        add_tests("state_fsync", {
            runargs = "audit",
            runenvs = {LOGGER_FAULT_POINT = "state_fsync", LOGGER_FAULT_ERRNO = "5"},
            timeout = 10
        })
        add_tests("state_rename", {
            runargs = "audit",
            runenvs = {LOGGER_FAULT_POINT = "state_rename", LOGGER_FAULT_ERRNO = "5"},
            timeout = 10
        })
    target_end()

    target("crash_recovery_test")
        set_kind("binary")
        set_default(false)
        add_files("tests/test_crash_recovery.c")
        add_deps("logger_test_support")
        add_cflags("-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", {force = true})
        for _, point in ipairs({
            "after_audit_fsync",
            "before_state_rename",
            "after_state_rename",
            "after_checkpoint_commit"
        }) do
            add_tests("crash_" .. point, {
                group = "process-crash",
                runargs = point,
                timeout = 10
            })
        end
    target_end()

    target("syslog_multi_test")
        set_kind("binary")
        set_default(false)
        add_files("tests/test_syslog_multi.c")
        add_deps("logger_test_support")
        add_cflags("-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", {force = true})
        add_tests("default", {timeout = 5})
    target_end()

    -- Fixed nine-case process-crash evidence set.
    target("audit_rotation_crash_regression")
        set_kind("binary")
        set_default(false)
        add_files("tests/regression/test_audit_rotation_crash.c")
        add_deps("logger_test_support")
        add_includedirs("src", "tests/regression")
        add_cflags("-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", {force = true})
        add_ldflags("-Wl,--wrap=logger_fault_crash_if_requested", {force = true})
        add_tests("rotation_crash_sha256", {
            group = "process-crash",
            runargs = {"rotation", "sha256"},
            timeout = 30
        })
    target_end()

    target("file_audit_regression")
        set_kind("binary")
        set_default(false)
        add_files("tests/regression/test_file_audit.c")
        add_deps("logger_test_support")
        add_includedirs("src", "tests/regression")
        add_cflags("-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", {force = true})
        for _, scenario in ipairs({"logger-busy", "state-busy", "audit-busy", "cwd"}) do
            add_tests("file_audit_" .. scenario, {runargs = scenario, timeout = 20})
        end
        for _, point in ipairs({
            "file_after_archive_rename",
            "file_after_archive_dirsync",
            "file_after_active_open",
            "file_after_active_dirsync"
        }) do
            add_tests("file_crash_sha256_" .. point, {
                group = "process-crash",
                runargs = {point, "sha256"},
                timeout = 20
            })
        end
    target_end()

    -- The QEMU guest deliberately links only the test-support archive.
    target("vm_powercut_guest")
        set_kind("binary")
        set_default(false)
        add_files("tests/vm_powercut.c")
        add_deps("logger_test_support")
        add_cflags("-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", {force = true})
        add_ldflags("-static", "-Wl,--wrap=logger_fault_crash_if_requested", {force = true})
    target_end()
end


if has_config("build_regression_tests") then
    -- Keep regression instrumentation on a separate same-source archive.
    -- Production artifacts remain fortified and are never linked to this target.
    target("logger_regression_support")
        set_kind("static")
        set_default(false)
        for _, source in ipairs(logger_sources) do
            add_files(source)
        end
        if has_config("legacy_fork") then
            add_files("src/logger_fork.c")
            add_defines("LOGGER_ENABLE_LEGACY_FORK_HELPER=1", {public = true})
        end
        add_cflags("-std=gnu11", "-fPIC", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                   "-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=0", {force = true})
        set_symbols("hidden")
        add_defines("LOGGER_ENABLE_FAULT_INJECTION=0")
        add_defines("LOGGER_STATIC_DEFINE=1", {public = true})
        add_syslinks("pthread", {public = true})
        add_includedirs("include", "$(builddir)/generated", {public = true})
    target_end()

    local regression_targets = {
        {"queue_mpsc_regression", "tests/regression/test_queue_mpsc.c"},
        {"metadata_capture_regression", "tests/regression/test_metadata_capture.c"},
        {"global_flush_regression", "tests/regression/test_global_flush.c"},
        {"stderr_sigpipe_regression", "tests/regression/test_stderr_sigpipe.c"},
        {"crypto_vectors_test", "tests/test_crypto_vectors.c"},
        {"audit_concurrency_regression", "tests/regression/test_audit_concurrency.c"},
        {"crypto_contract_regression", "tests/regression/test_crypto_contract.c"},
        {"crypto_chain_tool", "tests/regression/crypto_chain_tool.c"},
        {"audit_record_regression", "tests/regression/test_audit_record.c"},
        {"audit_recovery_strict_regression", "tests/regression/test_audit_recovery_strict.c"},
        {"audit_recovery_capacity", "tests/regression/test_audit_recovery_capacity.c"},
        {"audit_reader_regression", "tests/regression/test_audit_reader.c"},
        {"sha256_only_regression", "tests/regression/test_sha256_only.c"},

        -- Default-static parity needed by the full sanitizer profile.
        {"production_isolation_regression", "tests/regression/test_fault_isolation.c",
            {"connect"}},
        {"source_ownership_regression", "tests/regression/test_source_ownership.c",
            {"logger_format_line"}},
        {"destroy_status_regression", "tests/regression/test_destroy_status.c",
            {"close"}},
        {"host_format_regression", "tests/regression/test_host_format.c"},
        {"global_stress_regression", "tests/regression/test_global_stress.c"},
        {"bench_matrix", "tests/bench_matrix.c"},

        -- Link-time interception parity, group A: queue/flush and Audit I/O/state.
        {"queue_notify_regression", "tests/regression/test_queue_notify.c",
            {"pthread_cond_wait"}},
        {"flush_pending_regression", "tests/regression/test_flush_pending.c",
            {"logger_format_line", "pthread_cond_wait", "fsync"}},
        {"flush_watermark_regression", "tests/regression/test_flush_watermark.c",
            {"logger_format_line", "pthread_cond_wait"}},
        {"io_accounting_regression", "tests/regression/test_io_accounting.c",
            {"logger_format_line", "write", "writev", "fsync"}},
        {"resource_cleanup_regression", "tests/regression/test_resource_cleanup.c",
            {"free"}},
        {"audit_commit_regression", "tests/regression/test_audit_commit.c",
            {"audit_checkpoint_persist_at", "logger_log_sync_status", "write", "fsync",
             "logger_file_offset"}},
        {"audit_lifecycle_regression", "tests/regression/test_audit_lifecycle.c",
            {"logger_destroy_status", "logger_log_sync_status", "pthread_mutex_lock"}},
        {"checkpoint_io_regression", "tests/regression/test_checkpoint_io.c",
            {"write", "fsync", "close", "renameat"}},
        {"crypto_failure_regression", "tests/regression/test_crypto_failure.c",
            {"audit_digest_provider"}},
        {"audit_tail_io_regression", "tests/regression/test_audit_tail_io.c",
            {"write", "fsync", "ftruncate"}},
        {"audit_dirfd_regression", "tests/regression/test_audit_dirfd.c",
            {"stat"}},
        {"audit_entropy_regression", "tests/regression/test_audit_entropy.c",
            {"getrandom"}},

        -- Link-time interception parity, group B: process fork and global lifecycle.
        {"process_fork_regression", "tests/regression/test_process_fork.c",
            {"pthread_atfork", "pthread_mutex_lock", "pthread_rwlock_rdlock",
             "pthread_rwlock_wrlock", "pthread_once", "calloc", "free",
             "pthread_create", "write", "close", "vsnprintf", "vfprintf",
             "logger_vlog_internal"}},
        {"global_lifecycle_regression", "tests/regression/test_global_lifecycle.c",
            {"pthread_mutex_lock", "pthread_rwlock_rdlock", "pthread_rwlock_wrlock",
             "logger_create", "logger_destroy_status", "logger_file_write",
             "logger_file_reopen", "dprintf", "fsync"}},
        {"global_cancel_regression", "tests/regression/test_global_cancel.c",
            {"logger_create", "logger_destroy_status", "logger_file_write",
             "logger_file_reopen", "logger_queue_push", "logger_format_line",
             "pthread_cond_wait", "pthread_mutex_lock", "pthread_rwlock_rdlock",
             "dprintf"}},

        -- Link-time interception parity, group C: explicit instance/Console scope.
        {"explicit_scope_regression", "tests/regression/test_explicit_scope.c",
            {"logger_file_write", "logger_file_reopen", "logger_queue_push",
             "pthread_cond_wait", "pthread_create", "close", "fflush",
             "vfprintf", "vsnprintf", "pthread_mutex_lock",
             "pthread_setcancelstate", "logger_format_line"}},

        -- Link-time interception parity, group D: syslog backend/fault/config/compat.
        {"syslog_backend_regression", "tests/regression/test_syslog_backend.c"},
        {"syslog_fault_regression", "tests/regression/test_syslog_faults.c",
            {"socket", "connect", "send", "clock_gettime", "close"}},
        {"syslog_config_regression", "tests/regression/test_syslog_config.c",
            {"connect"}},
        {"logger_v1_consumer", "tests/compat/test_logger_v1_consumer.c",
            {"connect"}},
        {"syslog_fallback_regression", "tests/regression/test_syslog_fallback.c",
            {"logger_format_line"}},

        -- Link-time interception parity, group E: file backend ownership/rotation.
        {"file_backend_regression", "tests/regression/test_file_backend.c",
            {"openat", "fsync", "close", "clock_gettime",
             "logger_file_rename_noreplace", "unlinkat", "flock", "writev"}},
        {"file_platform_regression", "tests/regression/test_file_platform.c",
            {"syscall"}},
        {"file_global_regression", "tests/regression/test_file_global.c",
            {"logger_destroy_status", "pthread_mutex_lock"}},
        {"file_legacy_probe", "tests/regression/test_file_legacy_probe.c",
            {"clock_gettime"}},
        {"file_audit_close_regression", "tests/regression/test_file_audit_close.c",
            {"logger_create_reserved_file", "close"}}
    }

    for _, spec in ipairs(regression_targets) do
        target(spec[1])
            set_kind("binary")
            set_default(false)
            add_files(spec[2])
            add_deps("logger_regression_support")
            add_includedirs("src")
            add_cflags("-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", {force = true})
            if spec[3] then
                for _, symbol in ipairs(spec[3]) do
                    add_ldflags("-Wl,--wrap=" .. symbol, {force = true})
                end
            end
        target_end()
    end

    for _, name in ipairs({
        "queue_mpsc_regression",
        "metadata_capture_regression",
        "global_flush_regression",
        "crypto_vectors_test"
    }) do
        target(name)
            add_tests("default", {timeout = 15})
        target_end()
    end

    target("stderr_sigpipe_regression")
        for _, scenario in ipairs({"sync", "async", "preblocked"}) do
            add_tests(scenario, {runargs = scenario, timeout = 15})
        end
    target_end()

    target("audit_concurrency_regression")
        for _, scenario in ipairs({"lifetimes", "transactions"}) do
            add_tests(scenario, {runargs = scenario, timeout = 30})
        end
    target_end()

    target("crypto_contract_regression")
        for _, scenario in ipairs({"vectors", "boundaries", "invalid", "threads"}) do
            add_tests(scenario, {runargs = {scenario, "sha256"}, timeout = 30})
        end
    target_end()

    for _, name in ipairs({
        "queue_notify_regression",
        "flush_pending_regression",
        "flush_watermark_regression"
    }) do
        target(name)
            add_tests(name, {timeout = 15})
        target_end()
    end

    target("resource_cleanup_regression")
        add_tests("resource_cleanup_regression", {timeout = 15})
    target_end()

    target("io_accounting_regression")
        for _, scenario in ipairs({
            "drop", "fallback", "fallback-error", "async-error", "short-ok",
            "short-error", "file-fsync", "dir-fsync", "rotation-fsync",
            "spill-policy", "sync"
        }) do
            local timeout = scenario == "spill-policy" and 30 or 15
            add_tests("io_" .. scenario .. "_regression",
                      {runargs = scenario, timeout = timeout})
        end
    target_end()

    target("audit_commit_regression")
        for _, scenario in ipairs({
            "checkpoint-once", "offset-error", "checkpoint-persistent",
            "io-write", "io-partial", "io-fsync", "preflight",
            "stop-error", "start-error"
        }) do
            add_tests("audit_" .. scenario .. "_regression",
                      {runargs = scenario, timeout = 20})
        end
    target_end()

    target("audit_lifecycle_regression")
        for _, scenario in ipairs({
            "init-gate", "stop-gate", "teardown-race",
            "generation", "fork-guard", "cancel"
        }) do
            add_tests("audit_" .. scenario .. "_regression",
                      {runargs = scenario, timeout = 20})
        end
    target_end()

    target("audit_dirfd_regression")
        add_tests("audit_dirfd_parent_rename", {timeout = 20})
    target_end()

    target("checkpoint_io_regression")
        for _, scenario in ipairs({
            "ok", "write", "zero", "short-eintr",
            "fsync", "dir-fsync", "close", "rename"
        }) do
            add_tests("checkpoint_" .. scenario .. "_regression",
                      {runargs = scenario, timeout = 20})
        end
    target_end()

    target("crypto_failure_regression")
        for _, scenario in ipairs({
            "init-probe", "init-start", "write", "begin", "end", "stop",
            "verify", "verify-empty", "recover-probe", "recover-forward", "recover-init"
        }) do
            add_tests("crypto_sha256_" .. scenario,
                      {runargs = {scenario, "sha256"}, timeout = 20})
        end
        add_tests("crypto_none_explicit",
                  {runargs = {"none", "sha256"}, timeout = 20})
    target_end()

    target("audit_tail_io_regression")
        for _, scenario in ipairs({
            "ok", "write", "zero", "short-eintr",
            "evidence-fsync", "directory-fsync", "truncate", "active-fsync"
        }) do
            add_tests("tail_io_" .. scenario,
                      {runargs = scenario, timeout = 20})
        end
    target_end()

    target("audit_entropy_regression")
        add_tests("audit_entropy_failfast", {timeout = 15})
    target_end()

    target("audit_record_regression")
        for _, scenario in ipairs({
            "roundtrip", "bounds", "fuzz", "suffix", "duplicate", "unknown",
            "reorder", "escape", "escape-nul", "escape-print", "raw-control",
            "unclosed", "seq-zero", "seq-overflow", "seq-leading", "seq-negative",
            "txn-plus", "txn-overflow", "error-overflow", "error-underflow",
            "negative-zero", "phase", "result", "attempt-result", "missing-result",
            "empty-event", "empty-operation", "instance", "bad-prefix", "bad-date",
            "trailing-space", "crlf", "missing-lf", "short-hash", "long-hash",
            "hash-nonhex", "hash-upper", "oversize", "oversize-eof", "nul"
        }) do
            add_tests("record_sha256_" .. scenario,
                      {runargs = {scenario, "sha256"}, timeout = 30})
        end
    target_end()

    target("audit_recovery_strict_regression")
        for _, scenario in ipairs({
            "forward", "partial", "complete-no-lf", "oversize-complete",
            "oversize-eof", "malformed", "nul", "checkpoint-offset",
            "checkpoint-ahead", "checkpoint-seq", "before-checkpoint-corrupt",
            "names", "archives", "time-backwards", "active-larger", "no-active",
            "empty-active", "archive-partial", "missing-middle", "duplicate",
            "branch", "active-not-last", "lost-state-genesis",
            "lost-state-retained", "retained-anchor", "symlink", "fifo"
        }) do
            add_tests("recovery_sha256_" .. scenario,
                      {runargs = {scenario, "sha256"}, timeout = 30})
        end
    target_end()

    target("audit_reader_regression")
        for _, scenario in ipairs({
            "reader", "ok", "suffix", "second-line", "second-blank",
            "negative-offset", "large-offset", "leading-seq", "unknown-alg",
            "short-hash", "long-hash", "short-crc", "unknown-version",
            "missing-lf", "nul", "bad-crc"
        }) do
            add_tests("strict_reader_" .. scenario,
                      {runargs = scenario, timeout = 20})
        end
    target_end()

    target("sha256_only_regression")
        for _, scenario in ipairs({
            "provider", "config", "verify-empty", "verify-missing", "live"
        }) do
            add_tests("sha256_only_" .. scenario,
                      {runargs = scenario, timeout = 20})
        end
        local legacy_sm3 = path.join(os.projectdir(), "tests", "fixtures",
                                     "legacy_crypto", "sm3")
        for _, scenario in ipairs({
            "verify-history", "checkpoint-read", "checkpoint-write",
            "recover-id", "init-history", "init-missing-state",
            "init-relabel-state"
        }) do
            add_tests("sha256_only_" .. scenario,
                      {runargs = {scenario, legacy_sm3}, timeout = 20})
        end
    target_end()

    -- Uses the real production logger for the builtin-only boundary.
    target("crypto_builtin_only_regression")
        set_kind("binary")
        set_default(false)
        add_files("tests/regression/test_crypto_builtin_only.c")
        add_deps("logger")
        add_includedirs("src")
        add_cflags("-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                   {force = true})
        add_tests("crypto_builtin_sha256_lifecycle",
                  {runargs = {"lifecycle", "sha256"}, timeout = 30})
        add_tests("crypto_builtin_sha256_legacy", {
            runargs = {
                "legacy", "sha256",
                path.join(os.projectdir(), "tests", "fixtures",
                          "legacy_crypto", "sha256")
            },
            timeout = 30
        })
    target_end()

    target("production_isolation_regression")
        for _, scenario in ipairs({
            "file_write", "file_fsync", "file_rename", "file_ftruncate",
            "state_write", "state_fsync", "state_rename", "short-write",
            "syslog-path", "after_audit_fsync", "before_state_rename",
            "after_state_rename", "after_checkpoint_commit"
        }) do
            add_tests("production_isolation_" .. scenario,
                      {runargs = scenario, timeout = 15})
        end
    target_end()

    target("test_hooks_regression")
        set_kind("binary")
        set_default(false)
        add_files("tests/regression/test_fault_isolation.c")
        add_deps("logger_test_support")
        add_includedirs("src")
        add_defines("TEST_EXPECT_HOOKS=1")
        add_cflags("-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                   {force = true})
        add_ldflags("-Wl,--wrap=connect", {force = true})
        for _, scenario in ipairs({
            "file_write", "file_fsync", "file_rename", "file_ftruncate",
            "state_write", "state_fsync", "state_rename", "short-write",
            "syslog-path", "after_audit_fsync", "before_state_rename",
            "after_state_rename", "after_checkpoint_commit"
        }) do
            add_tests("test_hooks_" .. scenario,
                      {runargs = scenario, timeout = 15})
        end
    target_end()

    target("source_ownership_regression")
        for _, scenario in ipairs({
            "queue-copy", "bounds", "null-source", "log", "log-source"
        }) do
            add_tests("source_ownership_" .. scenario,
                      {runargs = scenario, timeout = 15})
        end
    target_end()

    target("destroy_status_regression")
        for _, scenario in ipairs({"null", "drain", "io-error", "close-error"}) do
            add_tests("destroy_status_" .. scenario,
                      {runargs = scenario, timeout = 15})
        end
    target_end()

    target("host_format_regression")
        add_tests("host_format_bounds", {timeout = 15})
    target_end()

    target("global_stress_regression")
        for _, scenario in ipairs({"init-race", "sync", "async"}) do
            add_tests("global_stress_" .. scenario,
                      {runargs = scenario, timeout = 30})
        end
    target_end()

    if has_config("build_shared") then
        target("global_shared_regression")
            set_kind("binary")
            set_default(false)
            add_files("tests/regression/test_global_stress.c")
            add_deps("logger")
            add_includedirs("src")
            add_defines("LOGGER_PUBLIC_ABI_TEST=1")
            add_cflags("-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                       {force = true})
            for _, scenario in ipairs({"init-race", "sync", "async"}) do
                add_tests("global_shared_" .. scenario,
                          {runargs = scenario, timeout = 30})
            end
        target_end()
    end

    target("bench_matrix")
        add_tests("benchmark_accounting_smoke",
                  {runargs = {"4", "1000", "64"}, timeout = 15})
    target_end()

    -- Console host regression deliberately links the real production logger.
    target("console_host_regression")
        set_kind("binary")
        set_default(false)
        add_files("tests/regression/test_console_host.c")
        add_deps("logger")
        add_includedirs("src")
        add_cflags("-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                   {force = true})
        for _, scenario in ipairs({"cancel", "reentry", "config-snapshot"}) do
            add_tests("console_host_" .. scenario,
                      {runargs = scenario, timeout = 20})
        end
    target_end()

    if has_config("build_shared") then
        target("liblogger_dlclose_test")
            set_kind("binary")
            set_default(false)
            add_files("tests/test_liblogger_dlclose.c")
            add_deps("logger", {inherit = false})
            add_includedirs("include", "$(builddir)/generated")
            add_syslinks("dl")
            add_cflags("-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                       {force = true})
            on_test(function (target, opt)
                local logger = target:dep("logger")
                assert(logger, "logger dependency is required")
                local testname = opt.name:gsub("[/\\>=<|%*]", "_")
                local rundir = path.join(target:autogendir(), "test-work", testname)
                os.tryrm(rundir)
                os.mkdir(rundir)
                local code, errors = os.execv(path.absolute(target:targetfile()),
                                              {path.absolute(logger:targetfile())}, {
                    try = true,
                    timeout = opt.run_timeout or 20000,
                    curdir = rundir,
                    envs = opt.runenvs
                })
                if code == 0 then
                    return true
                end
                return false, errors or ("exit code: " .. tostring(code))
            end)
            add_tests("liblogger_dlclose", {timeout = 20})
        target_end()
    end

    -- Host-owned integration: build the SDK as an order-only dependency and
    -- pass its exact shared-library path to the executable.
    target("example_sdk")
        set_kind("shared")
        set_default(false)
        add_files("examples/host_owned/example_sdk.c")
        add_includedirs("examples/host_owned", {public = true})
        add_cflags("-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                   {force = true})
    target_end()

    local function run_with_example_sdk(target, opt)
        local sdk = target:dep("example_sdk")
        assert(sdk, "example_sdk dependency is required")
        local args = table.wrap(opt.runargs or target:get("runargs"))
        local sdkfile = path.absolute(sdk:targetfile())
        if target:name() == "host_owned_example" then
            args = {sdkfile, "host.log"}
        else
            table.insert(args, sdkfile)
        end
        local testname = opt.name:gsub("[/\\>=<|%*]", "_")
        local rundir = path.join(target:autogendir(), "test-work", testname)
        os.tryrm(rundir)
        os.mkdir(rundir)
        local code, errors = os.execv(path.absolute(target:targetfile()), args, {
            try = true,
            timeout = opt.run_timeout or 15000,
            curdir = rundir,
            envs = opt.runenvs
        })
        if code == 0 then
            return true
        end
        return false, errors or ("exit code: " .. tostring(code))
    end

    target("host_owned_example")
        set_kind("binary")
        set_default(false)
        add_files("examples/host_owned/host.c")
        add_deps("logger")
        add_deps("example_sdk", {inherit = false})
        add_includedirs("examples/host_owned")
        add_syslinks("dl")
        add_cflags("-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                   {force = true})
        on_test(run_with_example_sdk)
        add_tests("host_owned_example", {timeout = 15})
    target_end()

    target("host_ownership_regression")
        set_kind("binary")
        set_default(false)
        add_files("tests/regression/test_host_ownership.c")
        add_deps("logger_regression_support")
        add_deps("example_sdk", {inherit = false})
        add_includedirs("src", "examples/host_owned")
        add_syslinks("dl")
        add_cflags("-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                   {force = true})
        add_ldflags("-Wl,--wrap=logger_format_line", {force = true})
        on_test(run_with_example_sdk)
        for _, scenario in ipairs({
            "quiet", "callback-only", "invalid-options", "shared-sync",
            "shared-async", "separate", "concurrent", "unload-pending",
            "message-data", "global-independent"
        }) do
            add_tests("host_ownership_" .. scenario,
                      {runargs = scenario, timeout = 15})
        end
    target_end()

    if has_config("legacy_fork") then
        target("fork_reinit_regression")
            set_kind("binary")
            set_default(false)
            add_files("tests/regression/test_fork_reinit.c")
            add_deps("logger_regression_support")
            add_includedirs("src")
            add_cflags("-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                       {force = true})
            for _, symbol in ipairs({
                "fork", "opendir", "close", "calloc", "pthread_atfork"
            }) do
                add_ldflags("-Wl,--wrap=" .. symbol, {force = true})
            end
            for _, scenario in ipairs({
                "async", "sync", "drain", "no-init", "console-only",
                "after-shutdown", "context", "raw-guard", "thread-busy",
                "explicit-busy", "explicit-only", "audit-busy", "proc-failure",
                "fork-failure", "io-failure", "close-failure",
                "creation-failure", "registration-failure", "earlier-handler",
                "nested", "repeat", "explicit-recreate", "audit-conflict"
            }) do
                add_tests("fork_reinit_" .. scenario,
                          {runargs = scenario, timeout = 20})
            end
        target_end()

    end

    -- Treat process-fork exit 77 as an accepted skip-equivalent result.
    target("process_fork_regression")
        on_test(function (target, opt)
            local targetfile = path.absolute(target:targetfile())
            local runargs = table.wrap(opt.runargs or target:get("runargs"))
            local rundir = opt.rundir
            if not rundir then
                local testname = opt.name:gsub("[/\\>=<|%*]", "_")
                rundir = path.join(target:autogendir(), "test-work", testname)
                os.tryrm(rundir)
                os.mkdir(rundir)
            end
            local code, errors = os.execv(targetfile, runargs, {
                try = true,
                timeout = opt.run_timeout or 12000,
                curdir = rundir,
                envs = opt.runenvs
            })
            if code == 0 or code == 77 then
                if code == 77 then
                    print("%s/%s skipped (exit 77)", target:name(), opt.name)
                end
                return true
            end
            return false, errors or ("exit code: " .. tostring(code))
        end)
        for _, scenario in ipairs({
            "global", "explicit", "console-only", "context-only", "audit-only",
            "verify-only", "emit-held", "progress-held", "console-held",
            "reader-held", "registration-window", "earlier-handler",
            "after-shutdown", "bypass-handler", "prefork",
            "register-global", "register-explicit", "register-console",
            "register-context", "register-audit", "register-verify"
        }) do
            add_tests("process_fork_" .. scenario,
                      {runargs = scenario, timeout = 12})
        end
    target_end()

    target("global_lifecycle_regression")
        for _, op in ipairs({
            "write", "flush", "flush-void", "reopen",
            "level", "dropped", "metrics", "io", "diagnostics", "file-metrics"
        }) do
            add_tests("global_generation_" .. op,
                      {runargs = {"generation", op}, timeout = 20})
        end
        for _, scenario in ipairs({
            "contract", "bootstrap-generation", "shutdown-generation",
            "start-gate", "stop-gate", "closed-readers", "stop-during-start",
            "double-stop", "shutdown-error", "shutdown-fsync",
            "bootstrap-stop", "child"
        }) do
            add_tests("global_" .. scenario,
                      {runargs = scenario, timeout = 20})
        end
        for _, scenario in ipairs({
            "bootstrap", "create", "write", "reopen", "destroy"
        }) do
            add_tests("global_reentry_" .. scenario,
                      {runargs = {"reentry", scenario}, timeout = 20})
        end
        for _, scenario in ipairs({
            "read", "control", "publish", "create", "stop"
        }) do
            add_tests("global_error_" .. scenario,
                      {runargs = {"acquire-error", scenario}, timeout = 20})
        end
    target_end()

    target("global_cancel_regression")
        for _, scenario in ipairs({
            "write", "queue", "flush", "flush-void", "reopen", "reader",
            "create", "create-fail", "shutdown", "bootstrap",
            "control-wait", "restore-policy"
        }) do
            add_tests("global_cancel_" .. scenario,
                      {runargs = scenario, timeout = 20})
        end
    target_end()

    target("explicit_scope_regression")
        for _, scenario in ipairs({
            "write", "source", "sync", "format", "queue", "fallback",
            "flush", "flush-void", "reopen", "syslog-metrics", "file-metrics",
            "create", "create-fail", "destroy"
        }) do
            add_tests("explicit_cancel_" .. scenario,
                      {runargs = {"cancel", scenario}, timeout = 15})
        end
        for _, scenario in ipairs({
            "print", "info", "warn", "error", "verbose", "debug", "source"
        }) do
            add_tests("console_cancel_" .. scenario,
                      {runargs = {"console-cancel", scenario}, timeout = 15})
        end
        for _, scenario in ipairs({
            "log", "source", "sync", "flush", "flush-void", "reopen",
            "destroy", "destroy-void", "create", "level", "state",
            "dropped", "metrics", "io", "diagnostics", "file-metrics", "syslog", "context-set",
            "context-clear", "context-get", "console", "console-debug",
            "console-init", "console-level", "console-color", "console-tty",
            "global", "global-write", "audit", "audit-status"
        }) do
            add_tests("explicit_reentry_" .. scenario,
                      {runargs = {"reentry", "write", scenario}, timeout = 10})
        end
        for _, origin in ipairs({
            "worker", "console", "create", "format", "reopen", "destroy"
        }) do
            for _, scenario in ipairs({
                "flush", "destroy", "create", "global", "audit", "console"
            }) do
                add_tests("explicit_" .. origin .. "_reentry_" .. scenario,
                          {runargs = {"reentry", origin, scenario}, timeout = 10})
            end
        end
        for _, scenario in ipairs({
            "restore-policy", "create-async-reject", "setup-failure",
            "stdio-first-error", "context-alias", "debug-overflow",
            "invalid-level"
        }) do
            add_tests("explicit_contract_" .. scenario,
                      {runargs = {"contract", scenario}, timeout = 15})
        end
        for _, scenario in ipairs({"global", "create"}) do
            add_tests("explicit_global-worker_reentry_" .. scenario,
                      {runargs = {"reentry", "global-worker", scenario},
                       timeout = 10})
        end
    target_end()

    target("syslog_backend_regression")
        for _, scenario in ipairs({
            "flags", "wire", "file-fails", "pressure", "multi-backend",
            "async-pressure", "reconnect", "deferred", "required-missing",
            "endpoint-type", "fd-churn", "init-cleanup", "isolation",
            "threads", "copy-path", "cwd", "default-tag", "max-tag",
            "long-tag", "tag-injection", "bad-facility", "bad-startup",
            "bad-interval", "empty-path", "long-path", "invalid-metrics",
            "fork-guard"
        }) do
            add_tests("syslog_backend_" .. scenario,
                      {runargs = scenario, timeout = 15})
        end
    target_end()

    if has_config("build_shared") then
        target("syslog_shared_regression")
            set_kind("binary")
            set_default(false)
            add_files("tests/regression/test_syslog_backend.c")
            add_deps("logger")
            add_includedirs("src")
            add_cflags("-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                       {force = true})
            for _, scenario in ipairs({
                "flags", "wire", "file-fails", "pressure", "multi-backend",
                "async-pressure", "reconnect", "deferred", "required-missing",
                "endpoint-type", "fd-churn", "init-cleanup", "isolation",
                "threads", "copy-path", "cwd", "default-tag", "max-tag",
                "long-tag", "tag-injection", "bad-facility", "bad-startup",
                "bad-interval", "empty-path", "long-path", "invalid-metrics",
                "fork-guard"
            }) do
                add_tests("syslog_shared_" .. scenario,
                          {runargs = scenario, timeout = 15})
            end
        target_end()
    end

    target("syslog_fault_regression")
        for _, scenario in ipairs({
            "socket-emfile", "socket-enfile", "socket-enomem", "socket-eintr",
            "connect-eacces", "connect-eperm", "connect-enoent",
            "connect-refused", "connect-inprogress", "connect-eintr",
            "connect-again", "connect-prototype", "deferred-permission",
            "deferred-cooldown", "backwards-clock", "clock-init",
            "send-again", "send-nobufs", "send-enomem", "send-msgsize",
            "send-eintr", "send-pipe", "send-reset", "send-notconn",
            "send-refused", "send-io", "eintr-then-success", "zero-send",
            "short-send", "clock-disconnect", "close-disconnect",
            "reconnect-failure", "oversize", "max-packet"
        }) do
            add_tests("syslog_fault_" .. scenario,
                      {runargs = scenario, timeout = 15})
        end
    target_end()

    target("syslog_config_regression")
        for _, scenario in ipairs({
            "prefix", "zero-prefix", "header-only",
            "unknown-header", "partial-tail", "future-tail"
        }) do
            add_tests("syslog_config_" .. scenario,
                      {runargs = scenario, timeout = 15})
        end
    target_end()

    target("logger_v1_consumer")
        add_tests("syslog_old_header_consumer", {timeout = 15})
    target_end()

    if has_config("build_shared") then
        target("logger_v1_shared_consumer")
            set_kind("binary")
            set_default(false)
            add_files("tests/compat/test_logger_v1_shared.c")
            add_deps("logger")
            add_cflags("-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                       {force = true})
            add_tests("syslog_old_header_shared_consumer", {timeout = 15})
        target_end()
    end

    target("syslog_fallback_regression")
        add_tests("syslog_fallback_pressure", {timeout = 15})
    target_end()

    target("file_backend_regression")
        for _, scenario in ipairs({
            "owner-same", "owner-process", "owner-race", "owner-exit",
            "owner-alias", "owner-close-other", "path-empty", "path-long",
            "name-long", "path-directory", "path-fifo", "path-symlink",
            "path-dangling", "path-hardlink", "path-reserved-logger-lock",
            "lock-symlink", "lock-hardlink",
            "lock-fifo", "device-rotate", "device-shared", "init-cleanup",
            "reopen-ok", "reopen-permission", "reopen-space", "reopen-emfile",
            "reopen-directory", "reopen-symlink", "reopen-sync",
            "reopen-dir-sync", "reopen-close", "collision", "clock-backwards",
            "collision-limit", "rotation-unsupported", "rotation-rename-error",
            "rotation-new-open", "rotation-dir-sync", "rotation-new-sync",
            "rotation-new-dir-sync", "cwd", "directory-rename",
            "active-replaced", "retention", "retention-error", "metrics-contract",
            "vectors", "eintr"
        }) do
            add_tests("file_" .. scenario,
                      {runargs = scenario, timeout = 20})
        end
    target_end()

    target("file_platform_regression")
        for _, scenario in ipairs({"real", "enosys", "einval", "eopnotsupp"}) do
            add_tests("file_noreplace_" .. scenario,
                      {runargs = scenario, timeout = 20})
        end
    target_end()

    target("file_global_regression")
        for _, scenario in ipairs({"repeat-no-effects", "teardown"}) do
            add_tests("file_global_" .. scenario,
                      {runargs = scenario, timeout = 20})
        end
    target_end()

    target("file_legacy_probe")
        for _, scenario in ipairs({
            "owner", "reopen", "collision", "cwd",
            "symlink", "hardlink", "global-repeat"
        }) do
            add_tests("file_blackbox_" .. scenario,
                      {runargs = scenario, timeout = 20})
        end
    target_end()

    target("file_audit_close_regression")
        add_tests("file_audit_close", {timeout = 20})
    target_end()
end
