# The only target that may include <jack/jack.h>. It does so in src/ only, and
# links JACK privately, so nothing that links this compiles against JACK.
find_package(PkgConfig REQUIRED)
pkg_check_modules(JACK REQUIRED IMPORTED_TARGET jack)

add_library(sonora_backend_jack STATIC src/jack_backend.cpp)
target_include_directories(sonora_backend_jack PUBLIC include)
target_link_libraries(sonora_backend_jack
  PUBLIC sonora_backend_api sonora_base
  PRIVATE sonora_driver_support PkgConfig::JACK)
