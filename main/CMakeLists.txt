set(UI_DIR ./ui)
file(GLOB_RECURSE UI_SRCS ${UI_DIR}/*.c)

set(APP_DIR ./app)
file(GLOB_RECURSE APP_SRCS ${APP_DIR}/*.c)

set(SETTINGS_DIR ./settings)
file(GLOB_RECURSE SETTINGS_SRCS ${SETTINGS_DIR}/*.c)

idf_component_register(
    SRCS
        "main.c"
        ${UI_SRCS}
        ${APP_SRCS}
        ${SETTINGS_SRCS}
    INCLUDE_DIRS
        "."
        ${UI_DIR}
        ${APP_DIR}
        ${SETTINGS_DIR}
    # Phrase files of the voice commands, installed on SPIFFS when missing (main/app/file_phrases.c)
    EMBED_TXTFILES
        "phrases/phrases_fr.txt"
        "phrases/phrases_en.txt"
        "phrases/phrases_nl.txt"
        "phrases/phrases_de.txt"
        "phrases/phrases_es.txt"
        "phrases/phrases_it.txt"
        # Models offered (AGENTS.INI), installed on SPIFFS when missing (main/app/llm.c)
        "agents/agents.ini"
        # Who the assistant is, per language (PERSO_FR.TXT...), installed on SPIFFS when missing (main/app/file_persona.c)
        "persona/persona_fr.txt"
        "persona/persona_en.txt"
        "persona/persona_nl.txt"
        "persona/persona_de.txt"
        "persona/persona_es.txt"
        "persona/persona_it.txt"
        # Texts of the assistant, per language (LANG_FR.TXT...), installed on SPIFFS when missing (main/app/file_lang.c)
        "lang/lang_fr.txt"
        "lang/lang_en.txt"
        "lang/lang_nl.txt"
        "lang/lang_de.txt"
        "lang/lang_es.txt"
        "lang/lang_it.txt"
        # Countries known by voice, names per language (COUNTRY.TXT), installed on SPIFFS when missing (main/app/file_country.c)
        "lang/country.txt"
)

set(MV_UF2_BIN_EXE ${COMPONENT_PATH}/generateuf2.py)
idf_build_get_property(build_dir BUILD_DIR)
set(nvs_src_file ${PROJECT_DIR}/factory_nvs/build/factory_nvs.bin)
set(nvs_dst_file ${build_dir}/uf2/factory_nvs.bin)

if(NOT EXISTS ${nvs_src_file})
    message(FATAL_ERROR "Please ensure that the 'factory_nvs' is built first, and make sure to keep the 'HMI Board Config' consistent.")
endif()

add_custom_target(nvs_uf2_bin ALL)
add_custom_command(
        TARGET nvs_uf2_bin
        COMMAND ${CMAKE_COMMAND} -E make_directory ${build_dir}/uf2
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${nvs_src_file} ${nvs_dst_file}
        COMMAND ${CMAKE_COMMAND} -E echo "Copying factory_nvs.bin to destination if it is different..."
        VERBATIM)

add_dependencies(flash nvs_uf2_bin)

partition_table_get_partition_info(size "--partition-name ota_0" "size")
partition_table_get_partition_info(offset "--partition-name ota_0" "offset")

if("${size}" AND "${offset}")
    esptool_py_flash_to_partition(flash "ota_0" "${nvs_dst_file}")
else()
    set(message "Failed to find ota_0 in partition table file"
    "Please add a line to the partition file.")
endif()

spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)

# Google Calendar (main/app/google_calendar.c): the OAuth client of the project's Google Cloud project, read from
# google_oauth_client.json at the root (downloaded from the Cloud console; never committed, see .gitignore). Without
# it, the calendar is not offered.
set(GCAL_JSON "${CMAKE_CURRENT_LIST_DIR}/../google_oauth_client.json")
set(GCAL_CLIENT_ID "")
set(GCAL_CLIENT_SECRET "")
if(EXISTS "${GCAL_JSON}")
    file(READ "${GCAL_JSON}" gcal_json)
    string(JSON gcal_kind MEMBER "${gcal_json}" 0)
    string(JSON GCAL_CLIENT_ID GET "${gcal_json}" ${gcal_kind} client_id)
    string(JSON GCAL_CLIENT_SECRET GET "${gcal_json}" ${gcal_kind} client_secret)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GCAL_JSON}")
    message(STATUS "Google Calendar: OAuth client found")
else()
    message(STATUS "Google Calendar: no google_oauth_client.json, the calendar is not offered")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/gcal_client.h"
    "/* Generated from google_oauth_client.json by main/CMakeLists.txt: do not edit, never commit */
"
    "#define GCAL_CLIENT_ID \"${GCAL_CLIENT_ID}\"
"
    "#define GCAL_CLIENT_SECRET \"${GCAL_CLIENT_SECRET}\"
")
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
