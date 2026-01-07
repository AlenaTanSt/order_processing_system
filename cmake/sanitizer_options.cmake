option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)

function(target_enable_sanitizers target_name)
  if(MSVC)
    message(STATUS "Sanitizers: MSVC toolchain detected. Skipping sanitizer flags for target '${target_name}'.")
    return()
  endif()

  if(ENABLE_TSAN AND (ENABLE_ASAN OR ENABLE_UBSAN))
    message(FATAL_ERROR "ENABLE_TSAN cannot be combined with ENABLE_ASAN or ENABLE_UBSAN.")
  endif()

  set(_compile_flags "")
  set(_link_flags "")

  if(ENABLE_ASAN)
    list(APPEND _compile_flags -fsanitize=address -fno-omit-frame-pointer)
    list(APPEND _link_flags    -fsanitize=address)
  endif()

  if(ENABLE_UBSAN)
    list(APPEND _compile_flags -fsanitize=undefined -fno-omit-frame-pointer)
    list(APPEND _link_flags    -fsanitize=undefined)
  endif()

  if(ENABLE_TSAN)
    list(APPEND _compile_flags -fsanitize=thread -fno-omit-frame-pointer)
    list(APPEND _link_flags    -fsanitize=thread)
  endif()

  if(_compile_flags)
    target_compile_options(${target_name} PRIVATE ${_compile_flags})
    target_link_options(${target_name} PRIVATE ${_link_flags})
  endif()
endfunction()
