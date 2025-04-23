cmake_minimum_required(VERSION 3.15)
project(TaskQueueX VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")

find_package(Threads REQUIRED)
find_package(RocksDB REQUIRED)
find_package(Boost REQUIRED COMPONENTS system thread)
find_package(nlohmann_json REQUIRED)

add_library(taskqueuex_core
    src/core/Task.cpp
    src/core/QueueManager.cpp
    src/core/ThreadPool.cpp
    src/persistence/Storage.cpp
    src/scheduler/Scheduler.cpp
    src/network/TCPServer.cpp
)

target_include_directories(taskqueuex_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(taskqueuex_core PUBLIC
    Threads::Threads
    RocksDB::rocksdb
    Boost::system
    Boost::thread
    nlohmann_json::nlohmann_json
)

add_executable(taskqueuex src/main.cpp)
target_link_libraries(taskqueuex PRIVATE taskqueuex_core)

enable_testing()
add_subdirectory(tests) 