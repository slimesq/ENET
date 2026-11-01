find_path(ENET_MYSQL_INCLUDE_DIR mysql.h PATH_SUFFIXES mysql mariadb REQUIRED)
find_library(ENET_MYSQL_CLIENT_LIBRARY NAMES mysqlclient mariadb REQUIRED)

file(GLOB LOGIN_SERVER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
add_executable(LoginSvr ${LOGIN_SERVER_SOURCES})
target_include_directories(LoginSvr PRIVATE "${ENET_MYSQL_INCLUDE_DIR}")
target_link_libraries(LoginSvr PRIVATE EdoyunNet "${ENET_MYSQL_CLIENT_LIBRARY}")
