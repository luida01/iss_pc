# SPDX-License-Identifier: GPL-2.0-or-later
# tlg: the bench that measures what the Vulkan driver costs when the port records its
# commands (docs/analysis/record-cost.md).
#
# It needs only the Vulkan loader and the pinned headers: no GPU library, no guest.
#   cmake -S port/tests/vkbench -B out/vkbench -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   ninja -C out/vkbench
cmake_minimum_required(VERSION 3.24)
project(tlggpu_vk_record_cost LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Vulkan REQUIRED)

# The pinned Vulkan headers (the same ones the port uses): the distro headers can lag
# behind the extensions the bench measures.
set(PORT_GPU ${CMAKE_CURRENT_SOURCE_DIR}/../../gpu)
if (EXISTS ${PORT_GPU}/third_party/vulkan-headers/include/vulkan/vulkan.hpp)
    include_directories(BEFORE SYSTEM ${PORT_GPU}/third_party/vulkan-headers/include)
endif()

find_program(GLSLANG_COMPILER NAMES glslang glslangValidator REQUIRED)

# The trivial shaders of the bench: SPIR-V inside a header (the port's own scheme).
set(SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/include)
file(MAKE_DIRECTORY ${SHADER_DIR})
set(SHADER_HEADERS "")
function(add_bench_shader FILENAME SYMBOL)
    set(OUT ${SHADER_DIR}/${SYMBOL}.h)
    add_custom_command(OUTPUT ${OUT}
        COMMAND ${GLSLANG_COMPILER} --target-env vulkan1.3 -o ${OUT} --vn ${SYMBOL}
                ${CMAKE_CURRENT_SOURCE_DIR}/${FILENAME}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${FILENAME} VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${OUT} PARENT_SCOPE)
endfunction()
add_bench_shader(tri.vert TRI_SPIRV_VERT)
add_bench_shader(tri.frag TRI_SPIRV_FRAG)

add_executable(vk-record-cost vk_record_cost.cpp ${SHADER_HEADERS})
target_include_directories(vk-record-cost PRIVATE ${SHADER_DIR})
target_link_libraries(vk-record-cost PRIVATE Vulkan::Vulkan)
target_compile_options(vk-record-cost PRIVATE -Wall -Wextra)
