find_package(Qt6 REQUIRED COMPONENTS Test)

function(irrigation_add_test name)
    qt_add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(${name} PRIVATE
        Qt6::Core Qt6::Network Qt6::HttpServer Qt6::Sql Qt6::Test
        KanoopCommonQt KanoopDatabaseQt KanoopPiQt
    )
    target_compile_options(${name} PRIVATE -Wextra -Wall -Werror)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

irrigation_add_test(tst_settings
    tst_settings.cpp
    ../src/irrigationsettings.cpp
)
