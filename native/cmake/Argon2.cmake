# The upstream 20190702 distribution has no CMake package. Build only its
# portable reference implementation from the hash-pinned source, with no SIMD
# compiler assumptions or extra process/runtime dependency.
include(FetchContent)
FetchContent_Declare(argon2_source
    URL https://github.com/P-H-C/phc-winner-argon2/archive/20190702.tar.gz
    URL_HASH SHA256=daf972a89577f8772602bf2eb38b6a3dd3d922bf5724d45e7f9589b5e830442c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(argon2_source)
add_library(nordssh_argon2 STATIC
    ${argon2_source_SOURCE_DIR}/src/argon2.c
    ${argon2_source_SOURCE_DIR}/src/core.c
    ${argon2_source_SOURCE_DIR}/src/blake2/blake2b.c
    ${argon2_source_SOURCE_DIR}/src/thread.c
    ${argon2_source_SOURCE_DIR}/src/encoding.c
    ${argon2_source_SOURCE_DIR}/src/ref.c)
target_include_directories(nordssh_argon2 PUBLIC ${argon2_source_SOURCE_DIR}/include)
find_package(Threads REQUIRED)
target_link_libraries(nordssh_argon2 PRIVATE Threads::Threads)
add_library(Argon2::argon2 ALIAS nordssh_argon2)
