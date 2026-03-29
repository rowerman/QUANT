# librdkafka via pkg-config
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(RDKAFKA QUIET rdkafka)
endif()

if(RDKAFKA_FOUND)
  set(RDKAFKA_LIBRARIES ${RDKAFKA_LIBRARIES})
  set(RDKAFKA_INCLUDE_DIRS ${RDKAFKA_INCLUDE_DIRS})
  message(STATUS "Found rdkafka: ${RDKAFKA_LIBRARIES}")
else()
  set(RDKAFKA_FOUND FALSE)
endif()
