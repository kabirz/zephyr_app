# 生成 fw_gitver.h: #define FW_GIT_VERSION "<6hex>"
# 编译期执行 git rev-parse 取 6 位 commit hash, 供 CAN/UDP 固件升级库
# 拼接版本字符串 "v<M>.<m>.<p>_<6hex>". 无条件生成 (不依赖签名 key).
#
# 调用方: apps/libs/CMakeLists.txt (zephyr_include_directories 由调用方添加)

execute_process(
    COMMAND git rev-parse --short=6 HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE _git_sha
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _git_rc
)

if(NOT _git_rc EQUAL 0 OR _git_sha STREQUAL "")
    set(_git_sha "000000")   # fallback: 无 git / 命令失败
endif()

message(STATUS "fw_gitver: FW_GIT_VERSION=${_git_sha}")

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/fw_gitver.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/fw_gitver.h
    @ONLY
)
