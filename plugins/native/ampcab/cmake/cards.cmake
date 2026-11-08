# The card check of ADR-0069 s5, included from ampcab/CMakeLists.txt.
# Paths are baked in as defaults so `ampcab-card-check` with no arguments
# checks what ships; brain/models/ is where the cards live.
set(_ampcab_dir ${CMAKE_CURRENT_SOURCE_DIR})
get_filename_component(_ampcab_repo ${_ampcab_dir}/../../.. ABSOLUTE)

add_executable(ampcab-card-check tools/card_check.cpp)
target_link_libraries(ampcab-card-check PRIVATE sonora_native_ampcab_cards sonora_json)
target_compile_definitions(ampcab-card-check PRIVATE
    AMPCAB_DEFAULT_MANIFEST="${_ampcab_dir}/captures/manifest.json"
    AMPCAB_DEFAULT_CARDS="${_ampcab_repo}/brain/models"
    AMPCAB_DEFAULT_FILES="${_ampcab_dir}/captures"
    AMPCAB_DEFAULT_DENY="${_ampcab_dir}/tools/ampcab_name_deny.txt")

add_test(NAME native_ampcab_card_check COMMAND ampcab-card-check)

add_executable(test_native_ampcab_card_check tools/test_card_check.cpp)
target_link_libraries(test_native_ampcab_card_check PRIVATE sonora_native_ampcab_cards)
add_test(NAME native_ampcab_card_check_fixtures
         COMMAND test_native_ampcab_card_check $<TARGET_FILE:ampcab-card-check>
                 ${_ampcab_dir}/tools/ampcab_name_deny.txt
                 ${CMAKE_CURRENT_BINARY_DIR}/card_check_fixture)
