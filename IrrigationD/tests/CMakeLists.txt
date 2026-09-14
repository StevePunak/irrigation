find_package(Qt6 REQUIRED COMPONENTS Test)

function(irrigation_add_test name)
    qt_add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(${name} PRIVATE
        Qt6::Core Qt6::Network Qt6::HttpServer Qt6::Sql Qt6::Test
        KanoopCommonQt KanoopDatabaseQt KanoopPiQt
    )
    target_compile_options(${name} PRIVATE -Wextra -Wall -Werror)
    target_compile_definitions(${name} PRIVATE IRRIGATION_DB_VERSION=${IRRIGATION_DB_VERSION})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

irrigation_add_test(tst_settings
    tst_settings.cpp
    ../src/irrigationsettings.cpp
)

irrigation_add_test(tst_irrigationdatasource
    tst_datasource.cpp
    ../src/database/irrigationdatasource.cpp
    ../src/database/irrigation.qrc
    ../src/model/zone.cpp
    ../src/model/program.cpp
    ../src/model/programstarttime.cpp
    ../src/model/programzone.cpp
    ../src/model/firedinstant.cpp
)

irrigation_add_test(tst_repository
    tst_repository.cpp
    ../src/database/irrigationdatasource.cpp
    ../src/database/irrigation.qrc
    ../src/model/zone.cpp
    ../src/model/program.cpp
    ../src/model/programstarttime.cpp
    ../src/model/programzone.cpp
    ../src/model/firedinstant.cpp
)

irrigation_add_test(tst_zonecontroller
    tst_zonecontroller.cpp
    ../src/zonecontroller.cpp
)

irrigation_add_test(tst_stopbutton
    tst_stopbutton.cpp
    ../src/stopbutton.cpp
)
# Poisons freed heap memory so a use-after-free (e.g. reading InputPin::errorText()
# after deleting the pin) fails deterministically instead of reading stale-but-correct memory.
set_tests_properties(tst_stopbutton PROPERTIES
    ENVIRONMENT "MALLOC_PERTURB_=165;GLIBC_TUNABLES=glibc.malloc.tcache_count=0"
)

irrigation_add_test(tst_programrunner
    tst_programrunner.cpp
    ../src/programrunner.cpp
    ../src/zonecontroller.cpp
    ../src/database/irrigationdatasource.cpp
    ../src/database/irrigation.qrc
    ../src/model/zone.cpp
    ../src/model/program.cpp
    ../src/model/programstarttime.cpp
    ../src/model/programzone.cpp
    ../src/model/firedinstant.cpp
)

irrigation_add_test(tst_scheduler
    tst_scheduler.cpp
    ../src/scheduler.cpp
    ../src/database/irrigationdatasource.cpp
    ../src/database/irrigation.qrc
    ../src/model/zone.cpp
    ../src/model/program.cpp
    ../src/model/programstarttime.cpp
    ../src/model/programzone.cpp
    ../src/model/firedinstant.cpp
)
