cmake_minimum_required(VERSION 3.10)
project(MempoolMedia)

set(CMAKE_CXX_STANDARD 17)

# 查找 FFmpeg 组件
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED
    libavformat libavcodec libavutil libswscale)

# 包含目录
include_directories(
    ${CMAKE_SOURCE_DIR}
    ${FFMPEG_INCLUDE_DIRS}
)

# 内存池核心库
add_library(mempool STATIC
    mempool/ConcurrentAlloc.cpp
    mempool/CentralCache.cpp
    mempool/PageCache.cpp
    mempool/ThreadCache.cpp
)

# 内存池适配层
add_library(media_alloc STATIC
    media/ffmpeg_allocator.cpp
)
target_link_libraries(media_alloc mempool)

# 转码 Demo（内存池版本）
add_executable(transcode demo/transcode.cpp)
target_link_libraries(transcode
    media_alloc
    mempool
    ${FFMPEG_LIBRARIES}
    pthread
)

# 转码 Demo（原生 malloc 版本）
add_executable(transcode_native demo/transcode_native.cpp)
target_link_libraries(transcode_native
    ${FFMPEG_LIBRARIES}
    pthread
)
