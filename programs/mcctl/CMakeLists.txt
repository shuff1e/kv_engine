add_executable(mcctl mcctl.cc $<TARGET_OBJECTS:mc_program_utils>)
target_link_libraries(mcctl mc_client_connection mcd_util platform)
add_sanitizers(mcctl)
install(TARGETS mcctl RUNTIME DESTINATION bin)
