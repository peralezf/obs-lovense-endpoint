cmake_minimum_required(VERSION 3.16)
project(obs-lovense-endpoint VERSION 1.0.0 LANGUAGES C CXX)

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(libobs REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(Boost REQUIRED)

find_path(WEBSOCKETPP_INCLUDE_DIR websocketpp/server.hpp)
find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp)
if(NOT WEBSOCKETPP_INCLUDE_DIR)
  message(FATAL_ERROR "websocketpp headers were not found")
endif()
if(NOT NLOHMANN_JSON_INCLUDE_DIR)
  message(FATAL_ERROR "nlohmann-json headers were not found")
endif()

add_library(smartcam-core STATIC
  src/smart_server.cpp
  src/lovense_websocket_common.cpp
)
target_include_directories(smartcam-core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
  PRIVATE ${WEBSOCKETPP_INCLUDE_DIR} ${NLOHMANN_JSON_INCLUDE_DIR} ${Boost_INCLUDE_DIRS}
)
target_link_libraries(smartcam-core
  PUBLIC OpenSSL::SSL OpenSSL::Crypto Threads::Threads
)

add_library(obs-lovense-endpoint MODULE src/endpoint.cpp)
target_link_libraries(obs-lovense-endpoint PRIVATE smartcam-core OBS::libobs)
set_target_properties(obs-lovense-endpoint PROPERTIES
  PREFIX ""
  OUTPUT_NAME "obs-lovense-endpoint"
)

install(TARGETS obs-lovense-endpoint
  LIBRARY DESTINATION bin/64bit
)
install(FILES obs.json DESTINATION .)
install(FILES data/locale/en-US.ini DESTINATION data/locale)
