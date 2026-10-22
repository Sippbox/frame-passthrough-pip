cmake_minimum_required(VERSION 3.20)
project(framepip VERSION 0.9.1 LANGUAGES C CXX)

# Builds on the Steam Frame itself (SteamOS, aarch64): SteamVR ships libopenvr_api.so there.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(STEAMVR_LIB_DIR "/opt/steamvr/bin/linuxarm64" CACHE PATH "Directory holding SteamVR's libopenvr_api.so")
find_library(OPENVR_API openvr_api PATHS ${STEAMVR_LIB_DIR} NO_DEFAULT_PATH REQUIRED)
find_package(Vulkan REQUIRED)
find_program(GLSLC glslc REQUIRED)

set(THIRD_PARTY ${CMAKE_SOURCE_DIR}/third_party)

# ---- driver_framepip: loaded by vrserver, lends XRService's camera buffers
add_library(driver_framepip SHARED src/driver/driver_framepip.cpp)
target_include_directories(driver_framepip PRIVATE src/common ${THIRD_PARTY}/openvr)
set_target_properties(driver_framepip PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
target_link_libraries(driver_framepip PRIVATE pthread)

# ---- framepip: the overlay app
set(SHADER_INC ${CMAKE_BINARY_DIR}/generated/reproject.comp.inc)
add_custom_command(
  OUTPUT ${SHADER_INC}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
  COMMAND ${GLSLC} --target-env=vulkan1.1 -O -mfmt=num -o ${SHADER_INC} ${CMAKE_SOURCE_DIR}/src/app/shaders/reproject.comp
  DEPENDS ${CMAKE_SOURCE_DIR}/src/app/shaders/reproject.comp
  COMMENT "Compiling reproject.comp")

add_executable(framepip
  src/app/autostart.cpp
  src/app/main.cpp
  src/app/calibration.cpp
  src/app/canvas.cpp
  src/app/input.cpp
  src/app/panel.cpp
  src/app/pip_renderer.cpp
  src/app/room_grab.cpp
  src/app/settings.cpp
  src/app/tracking_cameras.cpp
  ${SHADER_INC})
target_include_directories(framepip PRIVATE src/common ${THIRD_PARTY} ${THIRD_PARTY}/openvr ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(framepip PRIVATE FRAMEPIP_VERSION="${PROJECT_VERSION}")
target_link_libraries(framepip PRIVATE ${OPENVR_API} Vulkan::Vulkan pthread)
set_target_properties(framepip PROPERTIES BUILD_RPATH ${STEAMVR_LIB_DIR} INSTALL_RPATH ${STEAMVR_LIB_DIR})

# ---- install layout (also the AppImage payload)
#   bin/framepip, share/{icon.png,...}, driver/framepip/{driver.vrdrivermanifest,bin/linuxarm64/driver_framepip.so}
install(TARGETS framepip RUNTIME DESTINATION bin)
install(TARGETS driver_framepip LIBRARY DESTINATION driver/framepip/bin/linuxarm64)
install(FILES resources/driver/framepip/driver.vrdrivermanifest DESTINATION driver/framepip)
file(WRITE ${CMAKE_BINARY_DIR}/VERSION "${PROJECT_VERSION}\n")
install(FILES ${CMAKE_BINARY_DIR}/VERSION DESTINATION share)
install(FILES resources/icon.png resources/localspace-unlocked.png resources/localspace-locked.png DESTINATION share)
install(FILES resources/input/actions.json resources/input/bindings_frame_controller.json DESTINATION share/input)
install(FILES resources/fonts/AtkinsonHyperlegible-Regular.ttf resources/fonts/AtkinsonHyperlegible-Bold.ttf
        resources/fonts/OFL.txt resources/fonts/MaterialIcons-Regular.ttf resources/fonts/MaterialIcons-LICENSE.txt
        DESTINATION share/fonts)
install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION share)
