cmake_minimum_required(VERSION 3.10)


# 设置C++标准
set(CMAKE_CXX_STANDARD 17)


set(CMAKE_SYSTEM_NAME ANDROID) 
set(CMAKE_SYSTEM_VERSION 28) 
set(ANDROID_PLATFORM 28) 
set(ANDROID_ABI arm64-v8a) 

set(CMAKE_ANDROID_NDK D:/ndkollvm/android-ndk-r25c.Ollvm/android-ndk-r25c)
set(CMAKE_TOOLCHAIN_FILE ${CMAKE_ANDROID_NDK}/build/cmake/android.toolchain.cmake)


project(newscan VERSION 1.0.0 LANGUAGES CXX)
set(CMAKE_ANDROID_ARCH_ABI arm64-v8a)
set(ANDROID_ABI arm64-v8a)
set(ANDROID_PLATFORM android-26)
set(ANDROID_NDK ${CMAKE_ANDROID_NDK})

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY 
    ${CMAKE_SOURCE_DIR}/outputs/
) 


# 包含目录
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/memtool
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
    ${CMAKE_CURRENT_SOURCE_DIR}/threadpool
    ${CMAKE_CURRENT_SOURCE_DIR}/chainer
    )


# 源文件
file(GLOB_RECURSE SOURCES 
"memtool/memextend.cpp"
"utils/*.cpp"
"threadpool/threadpool.cpp"
)

add_executable(newscan ${SOURCES} main.cpp)



    