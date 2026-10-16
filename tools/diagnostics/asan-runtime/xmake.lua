-- 只验收检测工具，不编译或安装 Logger；项目测试仍由根 xmake.lua 定义。
set_project("logger_asan_runtime_qualification")
set_xmakever("3.1.1")
set_allowedplats("linux")
set_policy("build.sanitizer.address", true)
set_policy("build.sanitizer.undefined", true)

target("asan_runtime_probe")
    set_kind("binary")
    set_default(false)
    add_files("../asan_fork_probe.c", "../asan_atfork_control.c")
    add_cflags("-std=gnu11", "-g", "-O1", "-Wall", "-Wextra", "-Werror", {force = true})
    add_ldflags("-Wl,--wrap=pthread_atfork", {force = true})
    add_syslinks("pthread")
target_end()
target("sanitizer_detection_probe")
    set_kind("binary")
    set_default(false)
    add_files("../../../tests/probes/sanitizer_detection.c")
    add_cflags("-std=gnu11", "-g", "-O1", "-fno-sanitize-recover=undefined", {force = true})
target_end()
target("asan_runtime_qualification")
    set_kind("phony")
    set_default(false)
    add_deps("asan_runtime_probe", "sanitizer_detection_probe")
    add_tests("runtime_and_detection", {group = "asan-runtime", timeout = 320})
    on_test(function (target, opt)
        local out = path.join(os.projectdir(), "../../../asan-runtime-evidence")
        local args = {path.join(os.projectdir(), "../../../scripts/check_asan_runtime.py"),
            "--fork-binary", path.absolute(target:dep("asan_runtime_probe"):targetfile()),
            "--detection-binary", path.absolute(target:dep("sanitizer_detection_probe"):targetfile()),
            "--output", out}
        local code, errors = os.execv("python3", args, {try = true, timeout = 310000})
        return code == 0, errors
    end)
target_end()
