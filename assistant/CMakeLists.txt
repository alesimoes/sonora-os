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

add_executable(assistant_contract_test tests/test_contract.cpp)
target_link_libraries(assistant_contract_test PRIVATE sonora_assistant_contract)
add_test(NAME assistant_contract COMMAND assistant_contract_test)
