get_filename_component(destination "${EXECUTABLE}" DIRECTORY)
set(search_directories)

# CMake knows project DLLs as well as imported DLLs. Copy these first so the
# binary scan can follow VPGLoader.dll and find its private dependencies.
foreach(dll IN LISTS RUNTIME_DLLS)
    get_filename_component(directory "${dll}" DIRECTORY)
    get_filename_component(filename "${dll}" NAME)
    list(APPEND search_directories "${directory}")
    if(NOT directory STREQUAL destination)
        file(COPY_FILE "${dll}" "${destination}/${filename}" ONLY_IF_DIFFERENT)
    endif()
endforeach()
list(REMOVE_DUPLICATES search_directories)

# Third-party DLLs can depend on libraries absent from their CMake interfaces
# (for example OpenImageIO's codecs). Resolve those from the actual binaries.
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${EXECUTABLE}"
    DIRECTORIES ${search_directories}
    RESOLVED_DEPENDENCIES_VAR dependencies
    UNRESOLVED_DEPENDENCIES_VAR missing
    CONFLICTING_DEPENDENCIES_PREFIX conflicts
    PRE_EXCLUDE_REGEXES "[Aa][Pp][Ii]-[Mm][Ss]-.*" "[Ee][Xx][Tt]-[Mm][Ss]-.*"
    POST_EXCLUDE_REGEXES ".*[/\\][Ss][Yy][Ss][Tt][Ee][Mm]32[/\\].*"
)
if(missing)
    message(FATAL_ERROR "Missing runtime DLLs for ${EXECUTABLE}: ${missing}")
endif()
# A DLL may already be deployed in both the library and executable directories.
# Identical copies are fine; different binaries with one name need attention.
foreach(filename IN LISTS conflicts_FILENAMES)
    set(candidates "${conflicts_${filename}}")
    list(GET candidates 0 selected)
    file(SHA256 "${selected}" expected_hash)
    foreach(candidate IN LISTS candidates)
        file(SHA256 "${candidate}" candidate_hash)
        if(NOT candidate_hash STREQUAL expected_hash)
            message(FATAL_ERROR "Conflicting runtime DLL versions: ${candidates}")
        endif()
    endforeach()
    list(APPEND dependencies "${selected}")
endforeach()
foreach(dll IN LISTS dependencies)
    get_filename_component(directory "${dll}" DIRECTORY)
    get_filename_component(filename "${dll}" NAME)
    if(NOT directory STREQUAL destination)
        file(COPY_FILE "${dll}" "${destination}/${filename}" ONLY_IF_DIFFERENT)
    endif()
endforeach()
