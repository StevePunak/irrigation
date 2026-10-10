set(PROJ irrigationd)

set(IRRIGATION_DB_VERSION 1.3.0)

find_package(Qt6 6.7 REQUIRED COMPONENTS Core Network HttpServer Sql)

qt_standard_project_setup()

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

file(GLOB_RECURSE IRRIGATIOND_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB_RECURSE IRRIGATIOND_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")

qt_add_executable(${PROJ}
    ${IRRIGATIOND_SOURCES}
    ${IRRIGATIOND_HEADERS}
    src/database/irrigation.qrc
)

target_compile_definitions(${PROJ} PRIVATE
    IRRIGATION_DB_VERSION=${IRRIGATION_DB_VERSION}
)

target_include_directories(${PROJ} PRIVATE src)

target_link_libraries(${PROJ} PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::HttpServer
    Qt6::Sql
    KanoopCommonQt
    KanoopDatabaseQt
    KanoopPiQt
)

target_compile_options(${PROJ} PRIVATE -Wextra -Wall -Werror)

install(TARGETS ${PROJ} RUNTIME DESTINATION bin)

install(FILES systemd/irrigationd.service DESTINATION lib/systemd/system)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
