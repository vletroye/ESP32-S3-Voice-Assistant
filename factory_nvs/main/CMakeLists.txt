set(UI_DIR ./ui)
file(GLOB_RECURSE UI_SRCS ${UI_DIR}/*.c)

set(APP_DIR ./app)
file(GLOB_RECURSE APP_SRCS ${APP_DIR}/*.c)

idf_component_register(
    SRCS 
        "main.c"
        ${UI_SRCS}
        ${APP_SRCS}
    INCLUDE_DIRS 
        "."
        ${UI_DIR}
        ${APP_DIR}
)

# Default values of CONFIG.INI: those of the assistant's sdkconfig (main/Kconfig.projbuild), the only place where
# they are set; given to main.c with their names (CONFIG_ASSISTANT_..., CONFIG_VOLUME_LEVEL) in a generated header,
# assistant_defaults.h (a definition on the command line would be cut at a blank: "FR:Français, EN:English")
set(ASSISTANT_SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/../../sdkconfig")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ASSISTANT_SDKCONFIG}")
file(STRINGS "${ASSISTANT_SDKCONFIG}" ASSISTANT_LINES ENCODING UTF-8 REGEX "^CONFIG_(ASSISTANT_[A-Z_]+|VOLUME_LEVEL)=")
set(ASSISTANT_DEFAULTS "/* Generated from the assistant's sdkconfig by main/CMakeLists.txt: do not edit */\n")
foreach(name ASSISTANT_OPENAI_URL ASSISTANT_LANGUAGE ASSISTANT_CHAT_MODEL ASSISTANT_TTS_VOICE
        ASSISTANT_REPLY_SCROLL_CPS ASSISTANT_SCROLL_MODE VOLUME_LEVEL ASSISTANT_SCREEN_OFF_S
        ASSISTANT_END_OF_SPEECH_MS ASSISTANT_OPENROUTER_FREE ASSISTANT_FREE_MODELS
        ASSISTANT_CONFIRM_OFF ASSISTANT_SHOW_CLOCK ASSISTANT_SLEEP_AFTER_CMD
        ASSISTANT_CURRENCY ASSISTANT_LANGUAGES ASSISTANT_RADIO_TREBLE)
    set(found "")
    foreach(line ${ASSISTANT_LINES})
        if(line MATCHES "^CONFIG_${name}=(.*)$")
            set(found "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(found STREQUAL "")
        message(FATAL_ERROR "CONFIG_${name} not in ${ASSISTANT_SDKCONFIG}: build the assistant first")
    endif()
    string(APPEND ASSISTANT_DEFAULTS "#define CONFIG_${name} ${found}\n")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/assistant_defaults.h" "${ASSISTANT_DEFAULTS}")
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
