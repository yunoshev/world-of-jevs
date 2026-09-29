# World of Jevs runtime module. AzerothCore auto-collects src/*.cpp.
message(STATUS "mod-world-of-jevs: public runtime hook applied")
target_include_directories(modules PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include)
install(FILES ${CMAKE_CURRENT_LIST_DIR}/conf/mod_world_of_jevs.conf.dist
        DESTINATION ${CONF_DIR}/modules
        RENAME mod_world_of_jevs.conf)
