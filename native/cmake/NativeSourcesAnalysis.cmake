include_guard(GLOBAL)

set(VOID_ANALYSIS_SOURCES
    "${VOID_NATIVE_DIR}/analysis/cache/overlay_chunk.cpp"
    "${VOID_NATIVE_DIR}/analysis/cache/overlay_raster.cpp"
    "${VOID_NATIVE_DIR}/analysis/cache/overlay_text.cpp"
    "${VOID_NATIVE_DIR}/analysis/cache/vacache_store.cpp"
    "${VOID_NATIVE_DIR}/analysis/parsers/vac2_parser.cpp"
    "${VOID_NATIVE_DIR}/analysis/parsers/vachunk_parser.cpp"
    "${VOID_NATIVE_DIR}/analysis/analysis_manager.cpp"
    "${VOID_NATIVE_DIR}/analysis/analysis_generation_service.cpp"
    "${VOID_NATIVE_DIR}/analysis/analysis_overlay_track_registry.cpp"
    "${VOID_NATIVE_DIR}/analysis/analysis_session.cpp"
    "${VOID_NATIVE_DIR}/analysis/generators/bitstream_indexer.cpp"
    "${VOID_NATIVE_DIR}/analysis/generators/analysis_generator.cpp"
)
