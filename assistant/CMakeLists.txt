# The opt-in Claude layer (ADR-0155). The contract is header-only: the
# feature bundle, the request builder's and reply parser's declarations,
# the KeyStore and HttpTransport seams and their fakes. The pure core
# (assistant/src/core) and the one network implementation
# (assistant/src/net) are added as libraries of their own beside it.
add_library(sonora_assistant_contract INTERFACE)
target_include_directories(sonora_assistant_contract INTERFACE include)
# reply_parser.h produces a model::Plan (ADR-0038).
target_link_libraries(sonora_assistant_contract INTERFACE sonora_model)

add_library(sonora_assistant_core STATIC
    src/core/request_builder.cpp src/core/reply_parser.cpp src/core/assistant_core.cpp)
target_include_directories(sonora_assistant_core PUBLIC src/core)
target_link_libraries(sonora_assistant_core PUBLIC sonora_assistant_contract
                      PRIVATE sonora_json)

add_executable(assistant_core_test tests/test_core_assistant.cpp)
target_link_libraries(assistant_core_test PRIVATE sonora_assistant_core)
add_test(NAME assistant_core COMMAND assistant_core_test)

add_executable(assistant_service_contract_test tests/test_service_contract.cpp)
target_link_libraries(assistant_service_contract_test PRIVATE sonora_assistant_contract)
add_test(NAME assistant_service_contract COMMAND assistant_service_contract_test)

# Later tasks add the service implementation and its tests as directories of
# their own; this file needs no edit when they do (ADR-0226).
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/service/CMakeLists.txt)
  add_subdirectory(src/service)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/service/CMakeLists.txt)
  add_subdirectory(tests/service)
endif()

add_executable(assistant_contract_test tests/test_contract.cpp)
target_link_libraries(assistant_contract_test PRIVATE sonora_assistant_contract)
add_test(NAME assistant_contract COMMAND assistant_contract_test)

# The one network implementation (ADR-0155 section 6). POSIX only; https needs
# OpenSSL, and without it an https endpoint fails rather than going clear.
if(UNIX)
  add_library(sonora_assistant_net STATIC
    src/net/file_key_store.cpp src/net/socket_transport.cpp)
  target_link_libraries(sonora_assistant_net PUBLIC sonora_assistant_contract)
  find_package(OpenSSL QUIET)
  if(OpenSSL_FOUND)
    target_compile_definitions(sonora_assistant_net PRIVATE SONORA_ASSISTANT_TLS)
    target_link_libraries(sonora_assistant_net PRIVATE OpenSSL::SSL OpenSSL::Crypto)
  endif()
  add_executable(assistant_net_test tests/test_net_transport.cpp)
  target_link_libraries(assistant_net_test PRIVATE sonora_assistant_net)
  add_test(NAME assistant_net COMMAND assistant_net_test)
endif()
