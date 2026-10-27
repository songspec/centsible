# Fonts are embedded in the binary; the UI never depends on system fonts.
juce_add_binary_data(TMAssets HEADER_NAME TMAssets.h NAMESPACE TMAssets SOURCES
    ${CMAKE_SOURCE_DIR}/Assets/Fonts/SourceSansPro-Regular.otf
    ${CMAKE_SOURCE_DIR}/Assets/Fonts/SourceSansPro-Bold.otf)
set_target_properties(TMAssets PROPERTIES POSITION_INDEPENDENT_CODE TRUE)

# The processor and the editor (with the Beatswitch panel) are compiled into the plugin and into every test that drives the real thing.
set(CENTSIBLE_PLUGIN_CODE src/PluginProcessor.cpp src/PluginEditor.cpp src/BeatswitchPanel.cpp)
set(CENTSIBLE_PLUGIN_SOURCES
    ${CENTSIBLE_PLUGIN_CODE}
    src/PluginProcessor.h src/AnalysisThread.h src/FileAnalyzer.h
    src/PluginEditor.h src/TuningLookAndFeel.h
    src/BeatswitchPanel.h src/BeatswitchRunner.h)

juce_add_plugin(Centsible
    COMPANY_NAME "SongSpec"
    PLUGIN_MANUFACTURER_CODE Sspc
    PLUGIN_CODE Tmat
    FORMATS VST3 Standalone
    PRODUCT_NAME "Centsible"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Analyzer)
target_sources(Centsible PRIVATE ${CENTSIBLE_PLUGIN_SOURCES})
target_include_directories(Centsible PRIVATE src)
target_compile_definitions(Centsible PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(Centsible
    PRIVATE centsible_core juce::juce_audio_utils TMAssets
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# Runs the real processor: bit-identical passthrough and a live reading.
juce_add_console_app(centsible_passthrough_test PRODUCT_NAME "centsible_passthrough_test")
target_sources(centsible_passthrough_test PRIVATE ${CMAKE_SOURCE_DIR}/tests/passthrough_test.cpp ${CENTSIBLE_PLUGIN_CODE})
target_include_directories(centsible_passthrough_test PRIVATE src)
target_compile_definitions(centsible_passthrough_test PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(centsible_passthrough_test PRIVATE centsible_core juce::juce_audio_utils TMAssets juce::juce_recommended_config_flags)
add_test(NAME passthrough COMMAND centsible_passthrough_test)
set_tests_properties(passthrough PROPERTIES TIMEOUT 300)

# Beatswitch inside the plugin: live recording with a host tempo, bar | beat times, the tempo ramp, a dropped file, cancel, bad input.
juce_add_console_app(centsible_beatswitch_plugin_test PRODUCT_NAME "centsible_beatswitch_plugin_test")
target_sources(centsible_beatswitch_plugin_test PRIVATE ${CMAKE_SOURCE_DIR}/tests/beatswitch_plugin_test.cpp ${CENTSIBLE_PLUGIN_CODE})
target_include_directories(centsible_beatswitch_plugin_test PRIVATE src ${CMAKE_SOURCE_DIR}/tests ${CMAKE_SOURCE_DIR}/core/src)
target_compile_definitions(centsible_beatswitch_plugin_test PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(centsible_beatswitch_plugin_test PRIVATE centsible_core juce::juce_audio_utils TMAssets juce::juce_recommended_config_flags)
add_test(NAME beatswitch_plugin COMMAND centsible_beatswitch_plugin_test)
set_tests_properties(beatswitch_plugin PROPERTIES TIMEOUT 900)

# Renders the real editor offscreen (real processor, real analysis thread) to a PNG: used for the documentation screenshots.
juce_add_console_app(centsible_ui_snapshot PRODUCT_NAME "centsible_ui_snapshot")
target_sources(centsible_ui_snapshot PRIVATE ${CMAKE_SOURCE_DIR}/tests/ui_snapshot.cpp ${CENTSIBLE_PLUGIN_CODE})
target_include_directories(centsible_ui_snapshot PRIVATE src)
target_compile_definitions(centsible_ui_snapshot PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_USE_MP3AUDIOFORMAT=1 JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(centsible_ui_snapshot PRIVATE centsible_core juce::juce_audio_utils TMAssets juce::juce_recommended_config_flags)

# Host-style stress test: all sample rates / block sizes / layouts, repeated prepare, reset, NaN / Inf / clipping input.
juce_add_console_app(centsible_robustness_test PRODUCT_NAME "centsible_robustness_test")
target_sources(centsible_robustness_test PRIVATE ${CMAKE_SOURCE_DIR}/tests/robustness_test.cpp ${CENTSIBLE_PLUGIN_CODE})
target_include_directories(centsible_robustness_test PRIVATE src)
target_compile_definitions(centsible_robustness_test PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(centsible_robustness_test PRIVATE centsible_core juce::juce_audio_utils TMAssets juce::juce_recommended_config_flags)
add_test(NAME robustness COMMAND centsible_robustness_test)
set_tests_properties(robustness PROPERTIES TIMEOUT 900)
