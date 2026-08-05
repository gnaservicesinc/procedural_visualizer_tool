cmake_minimum_required(VERSION 3.20)
project(PvtInstalledConsumer LANGUAGES CXX)

find_package(ProceduralVisualizerTool CONFIG REQUIRED)

add_executable(pvt_installed_consumer main.cpp)
target_compile_features(pvt_installed_consumer PRIVATE cxx_std_17)
target_link_libraries(pvt_installed_consumer PRIVATE
    ProceduralVisualizerTool::ProceduralVisualizerTool)

