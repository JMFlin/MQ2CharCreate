include("${CMAKE_SOURCE_DIR}/src/Plugin.cmake")

find_package(fmt REQUIRED)
find_package(protobuf REQUIRED)

set(MQ2CharCreate_HEADERS "MQ2CharCreate.h")
source_groups("Header Files" ${MQ2CharCreate_HEADERS})

set(MQ2CharCreate_SOURCES "StateMachine.cpp" "MQ2CharCreate.cpp")
source_groups("Source Files" ${MQ2CharCreate_SOURCES})

add_library(MQ2CharCreate SHARED ${MQ2CharCreate_HEADERS} ${MQ2CharCreate_SOURCES})
set_target_properties(MQ2CharCreate PROPERTIES FOLDER "custom/plugins")

target_Plugin_props(MQ2CharCreate)

target_link_libraries(MQ2CharCreate PRIVATE fmt::fmt protobuf::libprotobuf)
target_link_libraries(MQ2CharCreate PRIVATE login MQ2Main)
