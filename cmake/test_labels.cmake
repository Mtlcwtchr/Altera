# Test layers. Every ctest test gets exactly one label:
#   fast - the default check layer (tools/check_fast.bat, ctest --preset sim-fast):
#          the sim's quick unit tests and combat. Target: about a minute in all.
#   full - everything else: the sim's multi-year worlds, engine, terrain, GPU, Python.
# Run a layer with `ctest -L fast` / `ctest -L full`, one test with `ctest -R <name>`.

# altera_sim_tests cases slower than ~5 s (name substrings, matched like the test
# exe's own filter). They run as the ctest test altera_sim_slow, label full; the
# rest of the suite is altera_sim, label fast. Measured 2026-11-20, RelWithDebInfo.
set(ALTERA_SIM_SLOW_TESTS
        altera_sim_farm_year_closes                   # ~320 s
        altera_sim_farm_harvests_differ_by_the_season # ~20 s
        altera_sim_crafts_chores_run)                 # ~13 s

# Tests in the fast layer; any other test is labelled full.
set(ALTERA_FAST_TESTS altera_sim altera_combat altera_combat_golden)

function(altera_add_sim_tests exe)
    set(excludes)
    foreach(t IN LISTS ALTERA_SIM_SLOW_TESTS)
        list(APPEND excludes "--exclude=${t}")
    endforeach()
    add_test(NAME altera_sim COMMAND ${exe} ${excludes})
    add_test(NAME altera_sim_slow COMMAND ${exe} ${ALTERA_SIM_SLOW_TESTS})
    set_tests_properties(altera_sim_slow PROPERTIES TIMEOUT 3600)
endfunction()

function(altera_label_tests)
    get_property(tests DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY TESTS)
    foreach(t IN LISTS tests)
        if(t IN_LIST ALTERA_FAST_TESTS)
            set_property(TEST ${t} APPEND PROPERTY LABELS fast)
        else()
            set_property(TEST ${t} APPEND PROPERTY LABELS full)
        endif()
    endforeach()
endfunction()
