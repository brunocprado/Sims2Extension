cmake_minimum_required(VERSION 3.20)
project(TS2SE_Coop_QoL VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(TS2_SOURCES
    src/proxy_ijl15.cpp
    src/hook_engine.cpp
    src/dx9_overlay.cpp
    src/dx9_postfx.cpp
    src/ui_kit.cpp
    src/ui_graphics_tab.cpp
    src/ts2se_engine.cpp
    src/coop_net.cpp
    src/engine_qol.cpp
)

add_library(ijl15 SHARED ${TS2_SOURCES} src/ijl15.def)
target_include_directories(ijl15 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(ijl15 PROPERTIES
    PREFIX ""
    OUTPUT_NAME "ijl15"
    SUFFIX ".dll"
)
