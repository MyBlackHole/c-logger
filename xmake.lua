set_project("prod_c_logger")
set_xmakever("2.8.5")
includes("@builtin/xpack")

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

set_allowedplats("linux")

local project_version = "0.9.3"
local abi_version = "0"

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
            PROJECT_VERSION_MAJOR = "0",
            PROJECT_VERSION_MINOR = "9",
            PROJECT_VERSION_PATCH = "3",
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
        configure_install_metadata(target, os.mkdir, io.writefile)

        target:add("installfiles", "API.md", "SECURITY.md", "TESTING.md", "CHANGELOG.md",
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

local package_kind = has_config("build_shared") and "shared" or "static"
xpack("logger_package")
    set_formats("targz")
    set_version(project_version)
    set_title("c-logger " .. project_version .. " controlled production candidate")
    set_description("Host-owned C Logger and Audit, builtin SHA-256")
    set_basename("prod-c-logger-" .. project_version .. "-Linux-x86_64-" .. package_kind)
    add_targets("logger")
    after_package(function (package)
        local output = package:outputfile()
        local digest = hash.sha256(output)
        io.writefile(output .. ".sha256",
                     digest .. "  " .. path.filename(output) .. "\n")
    end)
xpack_end()


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


if has_config("build_private_tests") then
    -- This archive intentionally carries the environment-driven test hooks.
    -- It is never installed and is never linked into the production logger.
    target("logger_test_support")
        set_kind("static")
        set_default(false)
        for _, source in ipairs(logger_sources) do
            add_files(source)
        end
        add_files("src/logger_fault.c")
        add_cflags("-std=gnu11", "-fPIC", "-Wall", "-Wextra", "-Wpedantic", "-Werror", {force = true})
        set_symbols("hidden")
        add_defines("LOGGER_ENABLE_FAULT_INJECTION=1")
        add_defines("LOGGER_STATIC_DEFINE=1", {public = true})
        add_syslinks("pthread", {public = true})
        add_includedirs("include", "$(builddir)/generated", {public = true})
    target_end()

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
            add_tests("crash_" .. point, {runargs = point, timeout = 10})
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

    -- Complete the same nine process-crash cases used by focused CMake CI.
    target("audit_rotation_crash_regression")
        set_kind("binary")
        set_default(false)
        add_files("tests/regression/test_audit_rotation_crash.c")
        add_deps("logger_test_support")
        add_includedirs("src", "tests/regression")
        add_cflags("-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", {force = true})
        add_ldflags("-Wl,--wrap=logger_fault_crash_if_requested", {force = true})
        add_tests("rotation_crash_sha256", {
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
        {"crypto_contract_regression", "tests/regression/test_crypto_contract.c"}
    }

    for _, spec in ipairs(regression_targets) do
        target(spec[1])
            set_kind("binary")
            set_default(false)
            add_files(spec[2])
            add_deps("logger_regression_support")
            add_includedirs("src")
            add_cflags("-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", {force = true})
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
end
