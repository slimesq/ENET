file(GLOB EDOYUN_NET_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
list(REMOVE_ITEM EDOYUN_NET_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")

add_library(EdoyunNet STATIC ${EDOYUN_NET_SOURCES})
target_include_directories(EdoyunNet PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(EdoyunNet PUBLIC Threads::Threads)
