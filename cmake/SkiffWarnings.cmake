# Applied to first-party targets only. SDK headers arrive as system includes, so their warnings
# never fail the build.
function(skiff_set_warnings target)
  target_compile_options(
    ${target}
    PRIVATE -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wconversion
            -Wsign-conversion
            -Wstrict-prototypes
            -Wmissing-prototypes
            -Wformat=2
            -Wundef
            -Wcast-align
            -Wnull-dereference)
  if(SKIFF_WARNINGS_AS_ERRORS)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()
