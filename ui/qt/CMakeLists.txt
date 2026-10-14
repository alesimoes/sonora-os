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

# 6.7 for QSGTextNode (scene-graph text in the timeline and rulers) and
# QFont::Tag (tabular figures). Debian trixie ships 6.8.
find_package(Qt6 6.7 QUIET COMPONENTS Core Gui Qml Quick Test)
if(NOT Qt6_FOUND)
  message(STATUS "ui/qt: Qt 6.7+ (Core Gui Qml Quick Test) not found; sonora-ui and its tests are skipped")
  return()
endif()
message(STATUS "ui/qt: building sonora-ui against Qt ${Qt6_VERSION}")

# Everything but main(), so the tests drive the same code the binary runs.
# The Qt-free parts (meter_model, key_router, control_adapter, and the
# Edit/Mix logic: model_access, timeline_document, edit_controller,
# insert_controller, timeline_scene, time_format, fader_taper, peak_source,
# theme) are kept free
# of Qt on purpose: they are what common/model and M7 touch, and what a test
# can check without a display.
add_library(sonora_ui_qt STATIC
  src/engine_adapter.h
  src/control_adapter.h src/control_adapter.cpp
  src/meter_model.h src/meter_model.cpp
  src/key_router.h src/key_router.cpp
  src/qt_keys.h src/qt_keys.cpp
  src/meter_item.h src/meter_item.cpp
  src/shell_model.h src/shell_model.cpp
  src/render_path.h src/render_path.cpp
  src/measure.h src/measure.cpp
  src/register_types.h
  src/theme.h src/theme_object.h
  src/model_access.h
  src/timeline_document.h src/timeline_document.cpp
  src/replica_document.h src/replica_document.cpp
  src/peak_source.h src/peak_source.cpp
  src/time_format.h src/time_format.cpp
  src/transport_model.h src/transport_model.cpp
  src/fader_taper.h src/fader_taper.cpp
  src/edit_controller.h src/edit_controller.cpp
  src/timeline_scene.h src/timeline_scene.cpp
  src/scene_item.h src/scene_item.cpp
  src/session_files.h src/session_files.cpp
  src/session_model.h src/session_model.cpp
  src/insert_controller.h src/insert_controller.cpp
  src/insert_model.h src/insert_model.cpp
)
set_target_properties(sonora_ui_qt PROPERTIES AUTOMOC ON)
target_include_directories(sonora_ui_qt PUBLIC src)
# FakeEngine: the engine behind the adapter when no real one is running,
# and in the tests (control_adapter.h).
target_link_libraries(sonora_ui_qt PUBLIC sonora_base sonora_keymap sonora_model sonora_api sonora_api_fake
  Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick)
# Only measure.cpp uses it, for per-thread CPU time (ADR-0008 V5).
target_link_libraries(sonora_ui_qt PRIVATE sonora_platform)
qt_add_resources(sonora_ui_qt sonora_qml
  PREFIX /sonora
  FILES qml/Main.qml qml/SButton.qml qml/TransportBar.qml qml/TransportWindow.qml qml/EditWindow.qml
        qml/MixWindow.qml qml/ChannelStrip.qml qml/InsertSlot.qml qml/PluginBrowser.qml
        qml/InsertPanel.qml
)

add_executable(sonora-ui src/main.cpp)
# ui/client: the socket to the real engine (M7). FakeEngine stays for when
# none is running, and for the tests.
target_link_libraries(sonora-ui PRIVATE sonora_ui_qt sonora_client sonora_platform)
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

# The Edit and Mix logic, without Qt: model operations and undo, gestures
# and keys into operations, zoom and scroll mapping, the fader taper, time
# formats, and which rectangle covers which pixel.
add_executable(test_ui_edit tests/test_edit.cpp)
target_link_libraries(test_ui_edit PRIVATE sonora_ui_qt)
target_compile_definitions(test_ui_edit PRIVATE
  SONORA_KEYMAP_DIR="${PROJECT_SOURCE_DIR}/keymaps")
add_test(NAME ui_edit COMMAND test_ui_edit)
set_tests_properties(ui_edit PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Counters, ruler, grid and snap through the whole tempo map; the Transport
# window's model on a scripted transport; then the window in the real QML,
# opened by Pro Tools' key and by its button.
add_executable(test_ui_transport tests/test_transport.cpp)
target_link_libraries(test_ui_transport PRIVATE sonora_ui_qt Qt6::Test)
target_compile_definitions(test_ui_transport PRIVATE
  SONORA_KEYMAP_DIR="${PROJECT_SOURCE_DIR}/keymaps")
add_test(NAME ui_transport COMMAND test_ui_transport)
set_tests_properties(ui_transport PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The Edit window's document as the engine's, on FakeEngine (ADR-0007 M7).
add_executable(test_ui_replica_document tests/test_replica_document.cpp)
target_link_libraries(test_ui_replica_document PRIVATE sonora_ui_qt)
add_test(NAME ui_replica_document COMMAND test_ui_replica_document)

# The same document on sonora-engine as a process, across lost connections.
# The engine is run, not linked (ADR-0007 §1).
if(UNIX AND TARGET sonora-engine)
  add_executable(test_ui_replica_reconnect tests/test_replica_reconnect.cpp)
  target_link_libraries(test_ui_replica_reconnect PRIVATE sonora_ui_qt sonora_client sonora_platform)
  target_compile_definitions(test_ui_replica_reconnect PRIVATE
    SONORA_ENGINE_BIN="$<TARGET_FILE:sonora-engine>")
  add_dependencies(test_ui_replica_reconnect sonora-engine)
  add_test(NAME ui_replica_reconnect COMMAND test_ui_replica_reconnect)
endif()

# Insert slots (ADR-0012 checklist 6) on FakeEngine: add, move, copy,
# bypass, remove, faults, parameters, undo, and File > Save flushing the
# plugins' state before it writes; then the Mix window's slots, chooser and
# panel loaded on the offscreen platform.
add_executable(test_ui_inserts tests/test_inserts.cpp)
target_link_libraries(test_ui_inserts PRIVATE sonora_ui_qt Qt6::Test)
add_test(NAME ui_inserts COMMAND test_ui_inserts)
set_tests_properties(ui_inserts PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_ui_shell tests/test_shell.cpp)
target_link_libraries(test_ui_shell PRIVATE sonora_ui_qt Qt6::Test)
target_compile_definitions(test_ui_shell PRIVATE
  SONORA_KEYMAP_DIR="${PROJECT_SOURCE_DIR}/keymaps")
add_test(NAME ui_shell COMMAND test_ui_shell)
set_tests_properties(ui_shell PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# test_render renders frames, so it needs a display and OpenGL: Xvfb and
# llvmpipe, on xcb. The offscreen platform would give the software scene
# graph, where meters draw nothing by design.
add_executable(test_ui_render tests/test_render.cpp)
target_link_libraries(test_ui_render PRIVATE sonora_ui_qt Qt6::Test)
# A private Xvfb per run, through tests/with-xvfb.sh, which says why it is
# not xvfb-run.
find_program(SONORA_XVFB Xvfb)
if(SONORA_XVFB)
  add_test(NAME ui_render
           COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/with-xvfb.sh 1280x720x24
                   $<TARGET_FILE:test_ui_render>)
  set_tests_properties(ui_render PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=xcb")
else()
  message(STATUS "ui/qt: Xvfb not found; ui_render (a rendered frame) is not run")
endif()
