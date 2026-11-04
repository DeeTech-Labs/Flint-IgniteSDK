# One source selection for host SDK and ESP-IDF consumers.
get_filename_component(FLINT_LUA_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../third_party/lua" ABSOLUTE)
file(GLOB FLINT_LUA_CORE CONFIGURE_DEPENDS "${FLINT_LUA_SOURCE_ROOT}/*.c")
list(FILTER FLINT_LUA_CORE EXCLUDE REGEX
    "/(lua|luac|linit|lcorolib|lbaselib|ldblib|liolib|loslib|loadlib|lmathlib|lstrlib|ltablib|lutf8lib)\\.c$")
set(FLINT_LUA_DEFINITIONS FLINT_ENABLE_LUA LUAI_MAXCCALLS=40)
