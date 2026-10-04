set(PROJ irrigationd)

set(IRRIGATION_DB_VERSION 1.2.0)

find_package(Qt6 6.7 REQUIRED COMPONENTS Core Network HttpServer Sql)

qt_standard_project_setup()

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

qt_add_executable(${PROJ}
    src/main.cpp
    src/irrigationsettings.h  src/irrigationsettings.cpp
    src/irrigationdaemon.h    src/irrigationdaemon.cpp
    src/zonecontroller.h      src/zonecontroller.cpp
    src/stopbutton.h          src/stopbutton.cpp
    src/panelformat.h         src/panelformat.cpp
    src/tm1637display.h       src/tm1637display.cpp
    src/runbutton.h           src/runbutton.cpp
    src/ipanelhost.h
    src/panelcontroller.h     src/panelcontroller.cpp
    src/panelhost.h           src/panelhost.cpp
    src/programrunner.h       src/programrunner.cpp
    src/programqueue.h        src/programqueue.cpp
    src/runrequest.h          src/runrequest.cpp
    src/iclock.h
    src/scheduler.h           src/scheduler.cpp
    src/irrigationcontrolserver.h  src/irrigationcontrolserver.cpp
    src/json/statusjson.h     src/json/statusjson.cpp
    src/json/programjson.h    src/json/programjson.cpp
    src/database/irrigationdatasource.h  src/database/irrigationdatasource.cpp
    src/database/irrigation.qrc
    src/model/zone.h              src/model/zone.cpp
    src/model/program.h           src/model/program.cpp
    src/model/programstarttime.h  src/model/programstarttime.cpp
    src/model/programstep.h       src/model/programstep.cpp
    src/model/firedinstant.h      src/model/firedinstant.cpp
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
