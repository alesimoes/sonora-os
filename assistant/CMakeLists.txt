# The opt-in Claude layer (ADR-0155). The contract is header-only: the
# feature bundle, the request builder's and reply parser's declarations,
# the KeyStore and HttpTransport seams and their fakes. The pure core
# (assistant/src/core) and the one network implementation
# (assistant/src/net) are added as libraries of their own beside it.
add_library(sonora_assistant_contract INTERFACE)
target_include_directories(sonora_assistant_contract INTERFACE include)
# reply_parser.h produces a model::Plan (ADR-0038).
target_link_libraries(sonora_assistant_contract INTERFACE sonora_model)

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
