# kairoConfig.cmake
# 供 find_package(kairo) 使用，与 kairoTargets.cmake、kairoConfigVersion.cmake 一同安装到
# ${CMAKE_INSTALL_LIBDIR}/cmake/kairo/

include(CMakeFindDependencyMacro)
find_dependency(Threads REQUIRED)

include("${CMAKE_CURRENT_LIST_DIR}/kairoTargets.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/kairoConfigVersion.cmake")