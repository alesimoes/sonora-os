# The interfaces' side of the control API (ADR-0007 §1, M7): SocketEngine,
# EngineControl over the engine's socket and its read-only state block.
# Links contracts/api and common/platform; no socket or mapping code of its
# own (ADR-0008 P9). Never engine/*.
add_library(sonora_client STATIC src/socket_engine.cpp)
target_include_directories(sonora_client PUBLIC include)
target_link_libraries(sonora_client PUBLIC sonora_api PRIVATE sonora_platform)

# The smallest interface: scripts and the container's end-to-end step.
add_executable(sonora-ctl tools/sonora_ctl.cpp)
target_link_libraries(sonora-ctl PRIVATE sonora_client)
install(TARGETS sonora-ctl RUNTIME DESTINATION bin)

# SocketEngine against the real engine as a separate process: the
# conformance suite as its third implementation, an interface killed
# mid-take, reconnection with undo and redo in step, malformed frames from a
# client. The engine is run, not linked: sonora-engine is a build-order
# dependency only, so ui/ still links nothing of engine/ (ADR-0007 §1).
if(UNIX AND TARGET sonora-engine)
  add_executable(test_socket_engine tests/test_socket_engine.cpp)
  target_link_libraries(test_socket_engine PRIVATE
    sonora_client sonora_platform sonora_api_conformance sonora_model)
  target_compile_definitions(test_socket_engine PRIVATE
    SONORA_ENGINE_BIN="$<TARGET_FILE:sonora-engine>")
  add_dependencies(test_socket_engine sonora-engine)
  add_test(NAME client_socket_engine COMMAND test_socket_engine)
endif()
