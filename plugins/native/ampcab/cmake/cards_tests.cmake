# The card checker end to end on synthetic fixtures (ADR-0069 s5), included
# from cards.cmake. Fixtures are written into the build tree by the test;
# tests/cards/ holds the static template card.
add_executable(test_native_ampcab_card_checker tests/test_capture_card_checker.cpp)
target_link_libraries(test_native_ampcab_card_checker PRIVATE
    sonora_native_ampcab_cards sonora_sha256)
add_test(NAME native_ampcab_card_checker
         COMMAND test_native_ampcab_card_checker $<TARGET_FILE:ampcab-card-check>
                 ${_ampcab_dir}/tools/ampcab_name_deny.txt
                 ${CMAKE_CURRENT_BINARY_DIR}/card_checker_cases
                 ${_ampcab_dir}/tests/cards)
