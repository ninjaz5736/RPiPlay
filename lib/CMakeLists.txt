cmake_minimum_required(VERSION 3.4.1)
include_directories( curve25519 ed25519 playfair llhttp )

aux_source_directory(. play_src)
set(DIR_SRCS ${play_src})

add_library( airplay
        STATIC
        ${DIR_SRCS}
        )

find_library( LIBPLIST NAMES plist plist-2.0 )

target_link_libraries( airplay
	    pthread
        curve25519
        ed25519
        playfair
        llhttp
        ${LIBPLIST} )

if( UNIX AND NOT APPLE )
  find_package(OpenSSL REQUIRED)
  target_link_libraries( airplay OpenSSL::Crypto )
  target_link_libraries( airplay dns_sd )
else()
  include_directories( /usr/local/opt/openssl@1.1/include/ )
  target_link_libraries( airplay /usr/local/opt/openssl@1.1/lib/libcrypto.a )
endif()
        
