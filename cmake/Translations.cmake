# Compatible with the Qt 6.5 minimum, including native/cross Windows builds.
# Compiled catalogs live in the executable, so all distribution methods use
# the same resources without filesystem lookup or a network service.
file(GLOB PVT_TRANSLATION_AUTHORING_FILES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/translations/pvt_*.ts")
set(PVT_TRANSLATION_FILES "${PROJECT_SOURCE_DIR}/translations/pvt_en.ts")
set(PVT_RELEASED_TRANSLATION_LOCALES)
set(PVT_TRANSLATION_PREVIEW_LOCALES "" CACHE STRING
    "Unreleased locale catalogs to embed in a local translator-review build")
file(STRINGS "${PROJECT_SOURCE_DIR}/translations/released-locales.txt"
    PVT_RELEASED_TRANSLATION_LOCALE_LINES)
foreach(locale IN LISTS PVT_RELEASED_TRANSLATION_LOCALE_LINES)
    string(STRIP "${locale}" locale)
    if(locale STREQUAL "" OR locale MATCHES "^#")
        continue()
    endif()
    if(NOT locale MATCHES "^[a-z][a-z][a-z]?(_[A-Za-z0-9]+)*$")
        message(FATAL_ERROR "Invalid released locale: ${locale}")
    endif()
    if(locale STREQUAL "en")
        message(FATAL_ERROR "English is implicit; do not list it in released-locales.txt")
    endif()
    list(FIND PVT_RELEASED_TRANSLATION_LOCALES "${locale}" duplicate_index)
    if(NOT duplicate_index EQUAL -1)
        message(FATAL_ERROR "Duplicate released locale: ${locale}")
    endif()
    set(catalog "${PROJECT_SOURCE_DIR}/translations/pvt_${locale}.ts")
    if(NOT EXISTS "${catalog}")
        message(FATAL_ERROR "Released locale has no catalog: ${catalog}")
    endif()
    list(APPEND PVT_RELEASED_TRANSLATION_LOCALES "${locale}")
    list(APPEND PVT_TRANSLATION_FILES "${catalog}")
endforeach()
set(PVT_COMPILED_TRANSLATION_LOCALES ${PVT_RELEASED_TRANSLATION_LOCALES})
set(PVT_SEEN_PREVIEW_TRANSLATION_LOCALES)
foreach(locale IN LISTS PVT_TRANSLATION_PREVIEW_LOCALES)
    if(NOT locale MATCHES "^[a-z][a-z][a-z]?(_[A-Za-z0-9]+)*$")
        message(FATAL_ERROR "Invalid preview locale: ${locale}")
    endif()
    if(locale STREQUAL "en")
        message(FATAL_ERROR "English is implicit; do not list it as a preview locale")
    endif()
    list(FIND PVT_SEEN_PREVIEW_TRANSLATION_LOCALES "${locale}" preview_duplicate_index)
    if(NOT preview_duplicate_index EQUAL -1)
        message(FATAL_ERROR "Duplicate preview locale: ${locale}")
    endif()
    list(APPEND PVT_SEEN_PREVIEW_TRANSLATION_LOCALES "${locale}")
    set(catalog "${PROJECT_SOURCE_DIR}/translations/pvt_${locale}.ts")
    if(NOT EXISTS "${catalog}")
        message(FATAL_ERROR "Preview locale has no catalog: ${catalog}")
    endif()
    list(FIND PVT_COMPILED_TRANSLATION_LOCALES "${locale}" compiled_index)
    if(compiled_index EQUAL -1)
        list(APPEND PVT_COMPILED_TRANSLATION_LOCALES "${locale}")
        list(APPEND PVT_TRANSLATION_FILES "${catalog}")
    endif()
endforeach()
set(PVT_UNRELEASED_TRANSLATION_LOCALES)
foreach(catalog IN LISTS PVT_TRANSLATION_AUTHORING_FILES)
    get_filename_component(locale "${catalog}" NAME_WE)
    string(REGEX REPLACE "^pvt_" "" locale "${locale}")
    if(locale STREQUAL "en")
        continue()
    endif()
    list(FIND PVT_COMPILED_TRANSLATION_LOCALES "${locale}" compiled_index)
    if(compiled_index EQUAL -1)
        # A removed release/preview catalog can otherwise survive an incremental build.
        file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/pvt_${locale}.qm")
    endif()
    list(FIND PVT_RELEASED_TRANSLATION_LOCALES "${locale}" released_index)
    if(released_index EQUAL -1)
        list(APPEND PVT_UNRELEASED_TRANSLATION_LOCALES "${locale}")
    endif()
endforeach()
file(GLOB PVT_TRANSLATION_SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/gui/*.cpp"
    "${PROJECT_SOURCE_DIR}/gui/*.h"
    "${PROJECT_SOURCE_DIR}/gui/*.mm")
add_custom_target(pvt_update_translations
    COMMAND Qt6::lupdate ${PVT_TRANSLATION_SOURCES}
        -locations relative -source-language en -no-obsolete
        -ts ${PVT_TRANSLATION_AUTHORING_FILES}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Updating all platforms' Qt translation catalogs"
    VERBATIM)
qt_add_lrelease(ProceduralVisualizerToolGui
    TS_FILES ${PVT_TRANSLATION_FILES}
    QM_FILES_OUTPUT_VARIABLE PVT_QM_FILES
    OPTIONS -nounfinished)
qt_add_resources(ProceduralVisualizerToolGui pvt_translations
    PREFIX "/i18n" BASE "${CMAKE_CURRENT_BINARY_DIR}"
    FILES ${PVT_QM_FILES})

# Resolve from the target SDK (including the KDE Snap SDK), not a build host's
# hardcoded Qt installation. Packagers can explicitly override this cache path.
find_path(PVT_QT_TRANSLATIONS_DIR qtbase_de.qm
    HINTS "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_TRANSLATIONS}"
          "${Qt6_DIR}/../../../translations"
    PATH_SUFFIXES translations share/qt6/translations)
if(NOT EXISTS "${PVT_QT_TRANSLATIONS_DIR}/qtbase_de.qm")
    message(FATAL_ERROR
        "Qt standard-dialog translations are required. Install Qt Translations "
        "(Debian/Ubuntu: qt6-translations-l10n) or set PVT_QT_TRANSLATIONS_DIR.")
endif()

file(GLOB PVT_QT_QM_FILES CONFIGURE_DEPENDS
    "${PVT_QT_TRANSLATIONS_DIR}/qtbase_*.qm")
qt_add_resources(ProceduralVisualizerToolGui pvt_qt_translations
    PREFIX "/i18n/qt" BASE "${PVT_QT_TRANSLATIONS_DIR}"
    FILES ${PVT_QT_QM_FILES})

# Declare localizations to Cocoa as well as Qt. Merely embedding .qm files
# does not describe the bundle's languages to macOS.
set(PVT_MACOS_LOCALIZATIONS "<string>en</string>")
foreach(catalog IN LISTS PVT_TRANSLATION_FILES)
    get_filename_component(locale "${catalog}" NAME_WE)
    string(REGEX REPLACE "^pvt_" "" locale "${locale}")
    if(NOT locale MATCHES "^[a-z][a-z][a-z]?(_[A-Za-z0-9]+)*$")
        message(FATAL_ERROR "Invalid translation filename: ${catalog}")
    endif()
    if(NOT locale STREQUAL "en")
        string(REPLACE "_" "-" locale "${locale}")
        string(APPEND PVT_MACOS_LOCALIZATIONS "\n        <string>${locale}</string>")
    endif()
endforeach()

if(APPLE)
    set(PVT_MACOS_TRANSLATION_FILES)
    foreach(locale en ${PVT_RELEASED_TRANSLATION_LOCALES})
        string(REPLACE "_" "-" macos_locale "${locale}")
        set(strings_file
            "${PROJECT_SOURCE_DIR}/translations/macos/${macos_locale}.lproj/InfoPlist.strings")
        if(NOT EXISTS "${strings_file}")
            message(FATAL_ERROR
                "Released locale ${locale} has no macOS InfoPlist.strings")
        endif()
        list(APPEND PVT_MACOS_TRANSLATION_FILES "${strings_file}")
        get_filename_component(locale_directory "${strings_file}" DIRECTORY)
        get_filename_component(locale_directory "${locale_directory}" NAME)
        set_source_files_properties("${strings_file}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources/${locale_directory}")
    endforeach()
    target_sources(ProceduralVisualizerToolGui PRIVATE ${PVT_MACOS_TRANSLATION_FILES})
    foreach(locale IN LISTS PVT_UNRELEASED_TRANSLATION_LOCALES)
        string(REPLACE "_" "-" macos_locale "${locale}")
        add_custom_command(TARGET ProceduralVisualizerToolGui POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E rm -rf
                "$<TARGET_BUNDLE_DIR:ProceduralVisualizerToolGui>/Contents/Resources/${macos_locale}.lproj"
            COMMENT "Removing unreleased ${locale} localization from the app bundle"
            VERBATIM)
    endforeach()
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_custom_target(pvt_check_translations
        COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/scripts/translations.py" check --lupdate "$<TARGET_FILE:Qt6::lupdate>"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" VERBATIM)
    add_dependencies(ProceduralVisualizerToolGui pvt_check_translations)
endif()

if(BUILD_TESTING)
    qt_add_executable(pvt_localization_tests
        tests/localization_test.cpp gui/localization.cpp gui/localization.h)
    target_include_directories(pvt_localization_tests PRIVATE gui)
    target_link_libraries(pvt_localization_tests PRIVATE Qt6::Widgets)
    pvt_enable_warnings(pvt_localization_tests)
    file(GLOB PVT_TEST_TS_FILES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/tests/translations/pvt_*.ts")
    set_source_files_properties(${PVT_TEST_TS_FILES} PROPERTIES
        OUTPUT_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/test-translations")
    qt_add_lrelease(pvt_localization_tests TS_FILES ${PVT_TEST_TS_FILES}
        QM_FILES_OUTPUT_VARIABLE PVT_TEST_QM_FILES OPTIONS -nounfinished)
    qt_add_resources(pvt_localization_tests pvt_test_translations
        PREFIX "/test-i18n" BASE "${CMAKE_CURRENT_BINARY_DIR}/test-translations"
        FILES ${PVT_TEST_QM_FILES})
    qt_add_resources(pvt_localization_tests pvt_test_qt_translations
        PREFIX "/i18n/qt" BASE "${PVT_QT_TRANSLATIONS_DIR}"
        FILES ${PVT_QT_QM_FILES})
    add_test(NAME pvt_localization COMMAND pvt_localization_tests)
    set_tests_properties(pvt_localization PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME pvt_released_localizations
        COMMAND "${CMAKE_COMMAND}"
            "-DPROGRAM=$<TARGET_FILE:ProceduralVisualizerToolGui>"
            -P "${PROJECT_SOURCE_DIR}/cmake/TestReleasedTranslations.cmake")
    set_tests_properties(pvt_released_localizations PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(Python3_Interpreter_FOUND)
        add_test(NAME pvt_translation_catalogs
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/translations.py" check
                --lupdate "$<TARGET_FILE:Qt6::lupdate>")
    endif()
endif()
