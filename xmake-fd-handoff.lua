-- 只追加有限反例测试；生产/回归归档完整复用根 xmake.lua，不复制构建定义。
includes("xmake.lua")

if has_config("build_regression_tests") then
    -- 健康与错误夹具使用相同生产实现；仅错误目标替换选定的移交操作。
    -- 三个可执行文件不安装，共享配置下也只链接同源回归静态归档。
    for _, spec in ipairs({{"healthy", 0}, {"output_copy", 1}, {"install_copy", 2}}) do
        target("fd_handoff_" .. spec[1])
            set_kind("binary")
            set_default(false)
            add_files("tests/regression/test_fd_handoff.c")
            add_deps("logger_regression_support")
            add_includedirs("src")
            add_defines("LOGGER_TEST_HANDOFF_VARIANT=" .. tostring(spec[2]),
                        "LOGGER_ENABLE_LOCKDEP=1", "LOGGER_ENABLE_FAULT_INJECTION=0")
            add_cflags("-std=gnu11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                       "-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=0", {force = true})
        target_end()
    end

    target("fd_handoff_verifier")
        set_kind("phony")
        set_default(false)
        add_deps("fd_handoff_healthy", "fd_handoff_output_copy", "fd_handoff_install_copy")
        add_tests("reject_nonconsuming_handoff", {group = "fd-handoff", timeout = 60})
        on_test(function (target, opt)
            local script = path.join(os.projectdir(), "tests", "scripts", "test_fd_handoff.py")
            local args = {script}
            for _, variant in ipairs({"healthy", "output_copy", "install_copy"}) do
                table.insert(args, path.absolute(target:dep("fd_handoff_" .. variant):targetfile()))
            end
            local code, errors = os.execv("python3", args, {
                try = true,
                timeout = opt.run_timeout or 60000
            })
            return code == 0, errors
        end)
    target_end()

end
