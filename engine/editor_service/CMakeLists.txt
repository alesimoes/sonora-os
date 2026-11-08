# engine/editor_service (ADR-0088): the interface the engine offers for
# opening a plug-in's own editor, and its in-memory fake. Header-only and
# std only, so engine/control and the UI's tests can both use it without a
# new module edge. The implementation over HostSupervisor is the follow-up
# feature plugins.host.editors.wire.
add_library(sonora_editor_service INTERFACE)
target_include_directories(sonora_editor_service INTERFACE include)
target_compile_features(sonora_editor_service INTERFACE cxx_std_17)

add_executable(test_plugin_editor_service tests/test_plugin_editor_service.cpp)
target_link_libraries(test_plugin_editor_service PRIVATE sonora_editor_service)
add_test(NAME plugin_editor_service COMMAND test_plugin_editor_service)
