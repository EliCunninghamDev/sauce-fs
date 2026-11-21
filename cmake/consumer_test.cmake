# Install the package into a scratch prefix, then configure and build a
# standalone project against it. Invoked by CTest with -D arguments; see
# CMakeLists.txt.

foreach(required SOURCE_DIR WORK_DIR PROJECT_BUILD_DIR DEPENDENCY_DIR GENERATOR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "consumer test: -D${required} is required")
    endif()
endforeach()

set(prefix_dir "${WORK_DIR}/prefix")
set(consumer_build "${WORK_DIR}/build")
set(prefix_path "${prefix_dir};${DEPENDENCY_DIR}")

file(REMOVE_RECURSE "${WORK_DIR}")

execute_process(
    COMMAND
        "${CMAKE_COMMAND}" --install "${PROJECT_BUILD_DIR}"
        --prefix "${prefix_dir}" --config "${BUILD_TYPE}"
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error
)
if(NOT install_result EQUAL 0)
    message(FATAL_ERROR
        "consumer test: install failed (${install_result})\n${install_output}${install_error}")
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        -S "${SOURCE_DIR}"
        -B "${consumer_build}"
        -G "${GENERATOR}"
        "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
        "-DCMAKE_PREFIX_PATH=${prefix_path}"
        -DCMAKE_SUPPRESS_REGENERATION=ON
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "consumer test: configure failed (${configure_result})\n"
                        "${configure_output}${configure_error}")
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}" --build "${consumer_build}"
        --config "${BUILD_TYPE}" --target check-consumer
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "consumer test: build failed (${build_result})\n"
                        "${build_output}${build_error}")
endif()
