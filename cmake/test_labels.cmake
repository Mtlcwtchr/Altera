# Test layers. Every ctest test gets exactly one label:
#   fast - the default check layer (tools/check_fast.bat, ctest --preset sim-fast):
#          the sim's quick unit tests and combat. Target: about a minute in all.
#   full - everything else: the sim's multi-year worlds, engine, terrain, GPU, Python.
# Run a layer with `ctest -L fast` / `ctest -L full`, one test with `ctest -R <name>`.

# lr_sim_tests cases slower than ~5 s (name substrings, matched like the test
# exe's own filter). They run as the ctest test lr_sim_slow, label full; the
# rest of the suite is lr_sim, label fast. Measured 2026-11-20, RelWithDebInfo.
set(LR_SIM_SLOW_TESTS
        lr_sim_farm_year_closes                   # ~320 s
        lr_sim_farm_harvests_differ_by_the_season # ~20 s
        lr_sim_crafts_chores_run                  # ~13 s
        lr_sim_village_lives_over_two_months      # ~36 s (AL-39)
        lr_sim_realm_lives_three_years            # ~15 s (AL-85)
        lr_sim_ecology_holds_without_people)      # AL-82, 3 years of beasts only

# Tests in the fast layer; any other test is labelled full.
set(LR_FAST_TESTS lr_sim lr_combat lr_combat_golden)

function(lr_add_sim_tests exe)
    set(excludes)
    foreach(t IN LISTS LR_SIM_SLOW_TESTS)
        list(APPEND excludes "--exclude=${t}")
    endforeach()
    add_test(NAME lr_sim COMMAND ${exe} ${excludes})
    add_test(NAME lr_sim_slow COMMAND ${exe} ${LR_SIM_SLOW_TESTS})
    set_tests_properties(lr_sim_slow PROPERTIES TIMEOUT 3600)
endfunction()

function(lr_label_tests)
    get_property(tests DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY TESTS)
    foreach(t IN LISTS tests)
        if(t IN_LIST LR_FAST_TESTS)
            set_property(TEST ${t} APPEND PROPERTY LABELS fast)
        else()
            set_property(TEST ${t} APPEND PROPERTY LABELS full)
        endif()
    endforeach()
endfunction()
