# The Qt 6 interface (ADR-0006, ADR-0007 §1). The only module that may include
# Qt, and nothing depends on it.
#
# Optional by construction: the engine's build container and CI have no Qt,
# and they must stay green. Without Qt 6 this directory says so once and adds
# nothing; tools/build-ui.sh builds and tests it in the sonora-ui image.
#
# Dynamically linked against the system Qt, never statically (ADR-0006,
# LGPL-3.0). Modules used: Core, Gui, Qml, Quick (LGPL-3.0) and Test
# (LGPL-3.0, tests only).

option(SONORA_BUILD_UI "Build the Qt interface when Qt 6 is available" ON)
if(NOT SONORA_BUILD_UI)
  message(STATUS "ui/qt: SONORA_BUILD_UI is off; sonora-ui not built")
  return()
endif()

find_package(Qt6 6.4 QUIET COMPONENTS Core Gui Qml Quick Test)
if(NOT Qt6_FOUND)
  message(STATUS "ui/qt: Qt 6 (Core Gui Qml Quick Test) not found; sonora-ui and its tests are skipped")
  return()
endif()
message(STATUS "ui/qt: building sonora-ui against Qt ${Qt6_VERSION}")

# Everything but main(), so the tests drive the same code the binary runs.
# The Qt-free parts (meter_model, key_router, synthetic_engine) are kept free
# of Qt on purpose: they are what M4 touches, and what a test can check
# without a display.
add_library(sonora_ui_qt STATIC
  src/engine_adapter.h
  src/synthetic_engine.h src/synthetic_engine.cpp
  src/meter_model.h src/meter_model.cpp
  src/key_router.h src/key_router.cpp
  src/qt_keys.h src/qt_keys.cpp
  src/meter_item.h src/meter_item.cpp
  src/shell_model.h src/shell_model.cpp
  src/render_path.h src/render_path.cpp
  src/measure.h src/measure.cpp
  src/register_types.h
)
set_target_properties(sonora_ui_qt PROPERTIES AUTOMOC ON)
target_include_directories(sonora_ui_qt PUBLIC src)
target_link_libraries(sonora_ui_qt PUBLIC sonora_keymap Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick)
qt_add_resources(sonora_ui_qt sonora_qml
  PREFIX /sonora
  FILES qml/Main.qml
)

add_executable(sonora-ui src/main.cpp)
target_link_libraries(sonora-ui PRIVATE sonora_ui_qt)
install(TARGETS sonora-ui RUNTIME DESTINATION bin)

# --- tests ------------------------------------------------------------------
# Headless. test_meter_model and test_qt_keys need no display at all;
# test_shell loads the real QML on the offscreen platform and drives it with
# key events, without rendering a frame.
add_executable(test_ui_meter_model tests/test_meter_model.cpp)
target_link_libraries(test_ui_meter_model PRIVATE sonora_ui_qt)
add_test(NAME ui_meter_model COMMAND test_ui_meter_model)

add_executable(test_ui_keys tests/test_qt_keys.cpp)
target_link_libraries(test_ui_keys PRIVATE sonora_ui_qt)
target_compile_definitions(test_ui_keys PRIVATE
  SONORA_KEYMAP_DIR="${PROJECT_SOURCE_DIR}/keymaps")
add_test(NAME ui_keys COMMAND test_ui_keys)
set_tests_properties(ui_keys PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_ui_shell tests/test_shell.cpp)
target_link_libraries(test_ui_shell PRIVATE sonora_ui_qt Qt6::Test)
target_compile_definitions(test_ui_shell PRIVATE
  SONORA_KEYMAP_DIR="${PROJECT_SOURCE_DIR}/keymaps")
add_test(NAME ui_shell COMMAND test_ui_shell)
set_tests_properties(ui_shell PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
