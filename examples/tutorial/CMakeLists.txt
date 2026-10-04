set(TUTORIAL_SOURCES
    01_first_task.cpp
    02_priority.cpp
    03_delayed_periodic.cpp
    04_batch.cpp
    05_dependencies.cpp
    06_observability.cpp
    07_realtime.cpp
    08_communication.cpp
    09_gpu.cpp
    10_service_data_import.cpp
    11_task_inputs.cpp
    12_blocking_io_worker.cpp
    13_cancellation_and_timers.cpp
    14_scheduling_runtime.cpp
)

set(TUTORIAL_TARGETS)

foreach(TUTORIAL_SOURCE IN LISTS TUTORIAL_SOURCES)
    get_filename_component(TUTORIAL_NAME "${TUTORIAL_SOURCE}" NAME_WE)
    set(TUTORIAL_TARGET "tutorial_${TUTORIAL_NAME}")

    add_executable(${TUTORIAL_TARGET} ${TUTORIAL_SOURCE})
    target_link_libraries(${TUTORIAL_TARGET} PRIVATE kairo)
    list(APPEND TUTORIAL_TARGETS ${TUTORIAL_TARGET})

    if(BUILD_TESTING)
        add_test(NAME ${TUTORIAL_TARGET} COMMAND ${TUTORIAL_TARGET})
        set_tests_properties(${TUTORIAL_TARGET} PROPERTIES
            LABELS "tutorial;smoke"
            TIMEOUT 15
        )
    endif()
endforeach()

add_custom_target(tutorial_examples DEPENDS ${TUTORIAL_TARGETS})
