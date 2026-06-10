cmake_minimum_required(VERSION 3.10)

project(SshUmdLoader LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSSH2 REQUIRED libssh2)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../Base/BaseLib
    ${CMAKE_CURRENT_BINARY_DIR}/BaseLib)

add_library(sshumdloader_core STATIC
    src/Core/Domain.cpp
    src/Core/UseCases.cpp
    src/Infrastructure/Adapters.cpp)
target_include_directories(sshumdloader_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${LIBSSH2_INCLUDE_DIRS})
target_compile_features(sshumdloader_core PUBLIC cxx_std_14)
target_link_libraries(sshumdloader_core PUBLIC BaseLib ${LIBSSH2_LIBRARIES})
target_compile_options(sshumdloader_core PRIVATE ${LIBSSH2_CFLAGS_OTHER})

add_library(sshumdloader_contracts INTERFACE)
target_link_libraries(sshumdloader_contracts INTERFACE sshumdloader_core)

add_executable(SshUmdLoader
    src/main.cpp)
target_link_libraries(SshUmdLoader PRIVATE sshumdloader_core)

add_executable(sshumdloader_core_smoke_test
    tests/CoreSmokeTest.cpp)
target_link_libraries(sshumdloader_core_smoke_test PRIVATE sshumdloader_core)
