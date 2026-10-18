-- 复用唯一生产构建定义；仅增加 Clang AST 检查，不改变普通测试集合。
includes("xmake.lua")

if has_config("build_private_tests") and has_config("build_regression_tests") then
    target("owned_fd_copy_check")
        set_kind("phony")
        set_default(false)
        add_deps("logger", "logger_regression_support", "logger_test_support")
        add_tests("reject_implicit_copies", {group = "owned-copy", timeout = 180})
        on_test(function (target, opt)
            import("core.tool.compiler")
            import("core.base.json")
            local root = os.projectdir()
            local evidence = os.getenv("LOGGER_OWNED_COPY_EVIDENCE") or
                             path.join(target:autogendir(), "owned-copy")
            os.mkdir(evidence)
            local units = {}
            -- 参数来自各目标的真实配置，不手写第二套源文件或宏清单。
            for _, name in ipairs({"logger", "logger_regression_support", "logger_test_support"}) do
                local library = target:dep(name)
                assert(compiler.name("c", {target = library}) == "clang", "Clang is required")
                for _, source in ipairs(library:sourcefiles()) do
                    table.insert(units, {
                        target = name, source = source, compiler = library:tool("cc"),
                        flags = compiler.compflags(source, {target = library})
                    })
                end
            end
            assert(#units > 0, "No source files")
            local manifest = path.join(evidence, "owned-copy-manifest.json")
            io.writefile(manifest, json.encode({schema = 1, directory = root, units = units}))
            local code, errors = os.execv("python3", {
                path.join(root, "tests/scripts/test_owned_fd_copies.py")
            }, {try = true, timeout = 60000,
                envs = {LOGGER_TEST_CLANG = target:dep("logger"):tool("cc")}})
            if code ~= 0 then
                return false, errors
            end
            local report = path.join(evidence, "owned-copy-report.json")
            code, errors = os.execv("python3", {
                path.join(root, "scripts/check_owned_fd_copies.py"), "--manifest", manifest
            }, {try = true, timeout = opt.run_timeout or 180000, stdout = report})
            print(io.readfile(report))
            return code == 0, errors
        end)
    target_end()
end
