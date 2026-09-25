set_project("prod_c_logger")
set_xmakever("2.8.2")

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

if not is_plat("linux") then
    raise("c-logger Xmake parity build currently supports Linux/ELF only")
end

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

local function generate_version_script(target)
    local manifest = path.join(os.scriptdir(), "cmake", "logger.symbols")
    local content = assert(io.readfile(manifest), "cannot read ABI manifest: " .. manifest)
    local out = {"LOGGER_0.9 {\n", "  global:\n"}

    for line in content:gmatch("[^\r\n]+") do
        local name = line:match("^%s*(.-)%s*$")
        if name ~= "" and name:sub(1, 1) ~= "#" then
            assert(name:match("^[a-z][a-z0-9_]+$"), "invalid ABI symbol in " .. manifest .. ": " .. name)
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
    return mapfile
end

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

    set_configdir("$(buildir)/generated")
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
    add_includedirs("include", "$(buildir)/generated", {public = true})

    on_load(function (target)
        if target:targetkind() == "shared" then
            local mapfile = generate_version_script(target)
            target:add("shflags", "-Wl,--version-script=" .. mapfile, {force = true})
            target:add("shflags", "-Wl,--no-undefined", {force = true})
            target:add("shflags", "-Wl,--no-undefined-version", {force = true})
        end
    end)
target_end()
