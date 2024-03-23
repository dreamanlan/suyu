# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(tests
    common/bit_field.cpp
    common/cityhash.cpp
    common/container_hash.cpp
    common/fibers.cpp
    common/host_memory.cpp
    common/param_package.cpp
    common/range_map.cpp
    common/ring_buffer.cpp
    common/scratch_buffer.cpp
    common/unique_function.cpp
    core/core_timing.cpp
    core/internal_network/network.cpp
    precompiled_headers.h
    video_core/memory_tracker.cpp
    input_common/calibration_configuration_job.cpp
)

create_target_directory_groups(tests)

target_link_libraries(tests PRIVATE common core input_common)
target_link_libraries(tests PRIVATE ${PLATFORM_LIBRARIES} Catch2::Catch2WithMain Threads::Threads)
if (WIN32 AND MSVC)
    set_target_properties(tests PROPERTIES LINK_FLAGS_DEBUG "/NODEFAULTLIB:libboost_context-vc141-mt-gd-x64-1_83.lib")
    set_target_properties(tests PROPERTIES LINK_FLAGS_RELEASE "/NODEFAULTLIB:libboost_context-vc141-mt-x64-1_83.lib")
endif()

add_test(NAME tests COMMAND tests)

if (SUYU_USE_PRECOMPILED_HEADERS)
    target_precompile_headers(tests PRIVATE precompiled_headers.h)
endif()
