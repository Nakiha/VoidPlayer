set(ANALYSIS_SOURCES
    parsers/vbt_parser.cpp
    parsers/vbi_parser.cpp
    parsers/vbs2_parser.cpp
    analysis_manager.cpp
)

add_library(analysis_lib STATIC ${ANALYSIS_SOURCES})

target_include_directories(analysis_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
