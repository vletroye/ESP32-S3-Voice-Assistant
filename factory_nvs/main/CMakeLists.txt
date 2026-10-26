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
# they are set; given to main.c with their names (CONFIG_ASSISTANT_..., CONFIG_VOLUME_LEVEL)
set(ASSISTANT_SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/../../sdkconfig")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ASSISTANT_SDKCONFIG}")
file(STRINGS "${ASSISTANT_SDKCONFIG}" ASSISTANT_LINES REGEX "^CONFIG_(ASSISTANT_[A-Z_]+|VOLUME_LEVEL)=")
foreach(name ASSISTANT_OPENAI_URL ASSISTANT_LANGUAGE ASSISTANT_CHAT_MODEL ASSISTANT_TTS_VOICE
        ASSISTANT_REPLY_SCROLL_CPS ASSISTANT_SCROLL_MODE VOLUME_LEVEL ASSISTANT_SCREEN_OFF_S
        ASSISTANT_END_OF_SPEECH_MS)
    set(found "")
    foreach(line ${ASSISTANT_LINES})
        if(line MATCHES "^CONFIG_${name}=(.*)$")
            set(found "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(found STREQUAL "")
        message(FATAL_ERROR "CONFIG_${name} not in ${ASSISTANT_SDKCONFIG}: build the assistant first")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "CONFIG_${name}=${found}")
endforeach()
