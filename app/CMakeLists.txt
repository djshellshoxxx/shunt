# JUCE applications. JUCE 8 on native toolchains, JUCE 7.0.12 for MinGW cross builds;
# app code must compile with both.
set(SHUNT_JUCE_DEFS JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0 JUCE_MODAL_LOOPS_PERMITTED=1 JUCE_STRICT_REFCOUNTEDPOINTER=1)

# Shared JUCE glue (MIDI sink factory over juce::MidiOutput, theme, paths)
add_subdirectory(common)
add_subdirectory(desktop)
add_subdirectory(headless)
