ADD_LIBRARY(mc_client_connection STATIC
            client_connection.h
            client_connection.cc
            client_connection_map.cc
            client_connection_map.h
            client_mcbp_commands.cc
            client_mcbp_commands.h)

TARGET_LINK_LIBRARIES(mc_client_connection
  PUBLIC
    mcbp
    cbsasl
    cbsocket
    mcd_tracing
    memcached_logger
    memcached_rbac
    ${COUCHBASE_NETWORK_LIBS}
    ${OPENSSL_LIBRARIES}
  PRIVATE
    cbcompress)
