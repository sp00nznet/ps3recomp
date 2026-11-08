# Build extensions.
#
# PORT_EXTENSIONS lists .cmake files to include once the game executable
# exists, with PORT_TARGET naming it. An extension may add sources,
# definitions, include directories and libraries to that target, and register
# runtime callbacks through port_extension.h. Empty by default, which leaves
# the build unchanged.
#
#   cmake -B build -DPORT_EXTENSIONS="/path/to/a.cmake;/path/to/b.cmake" ...
set(PORT_EXTENSIONS "" CACHE STRING
    "Build extension .cmake files included after the game target is defined")

function(port_apply_extensions target)
    set(PORT_TARGET "${target}")
    foreach(extension IN LISTS PORT_EXTENSIONS)
        if(NOT EXISTS "${extension}")
            message(FATAL_ERROR "PORT_EXTENSIONS entry not found: ${extension}")
        endif()
        message(STATUS "Build extension: ${extension}")
        include("${extension}")
    endforeach()
endfunction()
