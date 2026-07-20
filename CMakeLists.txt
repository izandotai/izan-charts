cmake_minimum_required(VERSION 3.28)
project(izan-charts LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    # -O0 miscompiles libstdc++ type_info comparisons under MinGW; an
    # optimized default is a load-bearing wall, not a preference.
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(MINGW)
    # Fully static: an executable may import Windows system libraries
    # and NOTHING else — no compiler-runtime DLLs ride along. DEP and
    # ASLR link bits set explicitly; mingw does not guarantee them.
    add_link_options(-static)
    add_link_options(-Wl,--nxcompat -Wl,--dynamicbase -Wl,--high-entropy-va)
    add_compile_definitions(_WIN32_WINNT=0x0A00)
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# The imgui stack arrives through izan-ui (its izan_imgui target:
# imgui + glfw + freetype, backends included). Keep pins in lockstep
# with other family members — the first declare wins.
FetchContent_Declare(izan_ui
    GIT_REPOSITORY https://github.com/izandotai/izan-ui.git
    GIT_TAG 0522ce8
    EXCLUDE_FROM_ALL
)
# implot, pinned by commit; the financial storey stands on it.
FetchContent_Declare(implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG d65a2bef53d32502407de3a4be80f191e2f412d7
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(izan_ui implot)

add_library(izan_implot STATIC
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
)
target_include_directories(izan_implot PUBLIC ${implot_SOURCE_DIR})
target_link_libraries(izan_implot PUBLIC izan_imgui)

# The charts: candles, indicators and linked panes — no window
# framework, no network, no opinion about where the data comes from.
add_library(izan_charts STATIC
    charts/charts.cpp
)
add_library(izan::charts ALIAS izan_charts)
target_include_directories(izan_charts PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(izan_charts PUBLIC izan_implot)

if(PROJECT_IS_TOP_LEVEL)
    # The acceptance shell: a random walk streamed live through the
    # full pane stack.
    add_executable(izan_charts_smoke WIN32 demo/smoke_main.cpp)
    target_link_libraries(izan_charts_smoke PRIVATE izan_charts)
endif()
