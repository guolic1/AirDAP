find_package(Git REQUIRED)

function(airdap_resolve_project_version repository output_variable)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --verify HEAD
        WORKING_DIRECTORY "${repository}"
        RESULT_VARIABLE hash_result
        OUTPUT_VARIABLE full_hash
        ERROR_VARIABLE hash_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT hash_result EQUAL 0)
        string(STRIP "${hash_error}" hash_error)
        message(FATAL_ERROR
            "cannot resolve the AirDAP firmware commit: ${hash_error}")
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" tag --points-at HEAD
        WORKING_DIRECTORY "${repository}"
        RESULT_VARIABLE tags_result
        OUTPUT_VARIABLE exact_tags_output
        ERROR_VARIABLE tags_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT tags_result EQUAL 0)
        string(STRIP "${tags_error}" tags_error)
        message(FATAL_ERROR
            "cannot inspect AirDAP firmware tags: ${tags_error}")
    endif()

    if(exact_tags_output)
        string(REPLACE "\r\n" "\n" exact_tags_output "${exact_tags_output}")
        string(REPLACE "\n" ";" exact_tags "${exact_tags_output}")
        list(LENGTH exact_tags exact_tag_count)
    else()
        set(exact_tag_count 0)
    endif()

    if(exact_tag_count GREATER 1)
        string(REPLACE ";" ", " exact_tags_display "${exact_tags}")
        message(FATAL_ERROR
            "multiple tags point at the AirDAP firmware commit: "
            "${exact_tags_display}")
    elseif(exact_tag_count EQUAL 1)
        list(GET exact_tags 0 resolved_version)
    else()
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
            WORKING_DIRECTORY "${repository}"
            RESULT_VARIABLE short_hash_result
            OUTPUT_VARIABLE resolved_version
            ERROR_VARIABLE short_hash_error
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(NOT short_hash_result EQUAL 0)
            string(STRIP "${short_hash_error}" short_hash_error)
            message(FATAL_ERROR
                "cannot shorten the AirDAP firmware commit: ${short_hash_error}")
        endif()
    endif()

    string(LENGTH "${resolved_version}" version_length)
    if(version_length GREATER 31)
        message(FATAL_ERROR
            "AirDAP firmware version '${resolved_version}' exceeds the "
            "31-byte ESP-IDF application version limit")
    endif()

    if(NOT CMAKE_SCRIPT_MODE_FILE)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --absolute-git-dir
            WORKING_DIRECTORY "${repository}"
            OUTPUT_VARIABLE git_directory
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" symbolic-ref -q HEAD
            WORKING_DIRECTORY "${repository}"
            RESULT_VARIABLE symbolic_ref_result
            OUTPUT_VARIABLE symbolic_ref
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        set(version_dependencies
            "${git_directory}/HEAD"
            "${git_directory}/refs/tags"
        )
        if(EXISTS "${git_directory}/packed-refs")
            list(APPEND version_dependencies "${git_directory}/packed-refs")
        endif()
        if(symbolic_ref_result EQUAL 0 AND EXISTS "${git_directory}/${symbolic_ref}")
            list(APPEND version_dependencies "${git_directory}/${symbolic_ref}")
        endif()
        set_property(DIRECTORY APPEND PROPERTY
            CMAKE_CONFIGURE_DEPENDS ${version_dependencies})
    endif()

    set(${output_variable} "${resolved_version}" PARENT_SCOPE)
endfunction()
