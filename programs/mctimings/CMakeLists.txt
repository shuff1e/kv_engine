add_executable(mctimings mctimings.cc $<TARGET_OBJECTS:mc_program_utils>)
target_link_libraries(mctimings mc_client_connection platform)
add_sanitizers(mctimings)
install(TARGETS mctimings RUNTIME DESTINATION bin)
