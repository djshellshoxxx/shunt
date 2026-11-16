# ctest ui_build_checks (FS-00 section 6): runs build_ui.cmake on the real ui/ (must pass) and on
# copies with a duplicate page id, a missing core module and a misnamed reserved module (must
# fail with the expected message). Original Shunt code.
# Usage: cmake -DUI_DIR=<ui> -DWORK=<scratch dir> -DBUILD_UI=<build_ui.cmake> -P test_build_ui.cmake
cmake_minimum_required(VERSION 3.16)

function(run_case name dir expect_ok expect_regex)
  execute_process(COMMAND ${CMAKE_COMMAND} -DUI_DIR=${dir} -DOUT=${WORK}/${name}.html -P ${BUILD_UI}
                  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(expect_ok AND NOT rc EQUAL 0)
    message(FATAL_ERROR "case ${name}: expected success, got ${rc}: ${err}")
  endif()
  if(NOT expect_ok AND rc EQUAL 0)
    message(FATAL_ERROR "case ${name}: expected failure, build_ui succeeded")
  endif()
  if(NOT "${out}${err}" MATCHES "${expect_regex}")
    message(FATAL_ERROR "case ${name}: output does not match '${expect_regex}': ${out}${err}")
  endif()
  message(STATUS "case ${name}: ok")
endfunction()

file(REMOVE_RECURSE ${WORK})
file(MAKE_DIRECTORY ${WORK})

# 1. the real UI builds, and the result has the shell, all six pages and no placeholder left
run_case(real ${UI_DIR} TRUE "[0-9]+ pages")
file(READ ${WORK}/real.html html)
foreach(needle "<title>Shunt</title>" "id:'live'" "id:'diagnostics'" "Shunt.slot('live-valve')")
  string(FIND "${html}" "${needle}" pos)
  if(pos EQUAL -1)
    message(FATAL_ERROR "real.html lacks ${needle}")
  endif()
endforeach()
foreach(ph "/*@CSS@*/" "/*@JS@*/")
  string(FIND "${html}" "${ph}" pos)
  if(NOT pos EQUAL -1)
    message(FATAL_ERROR "real.html still has placeholder ${ph}")
  endif()
endforeach()

# 2. duplicate page id
file(COPY ${UI_DIR}/ DESTINATION ${WORK}/dup)
file(WRITE ${WORK}/dup/js/60-onair.js "Shunt.registerPage({id:'live',title:'Again',mount(){},update(){}});\n")
run_case(dup ${WORK}/dup FALSE "duplicate page id")

# 3. registerPage without a literal first id key
file(COPY ${UI_DIR}/ DESTINATION ${WORK}/noid)
file(WRITE ${WORK}/noid/js/61-keyclash.js "const p={id:'kc'};Shunt.registerPage(p);\n")
run_case(noid ${WORK}/noid FALSE "must start")

# 4. missing core module
file(COPY ${UI_DIR}/ DESTINATION ${WORK}/missing)
file(REMOVE ${WORK}/missing/js/30-scope.js)
run_case(missing ${WORK}/missing FALSE "file missing")

# 5. reserved number used by another id
file(COPY ${UI_DIR}/ DESTINATION ${WORK}/reserved)
file(WRITE ${WORK}/reserved/js/64-other.js "// nothing\n")
run_case(reserved ${WORK}/reserved FALSE "reserved for 'replay'")

# 6. a feature module with its own page and CSS builds (spaces around the id are allowed)
file(COPY ${UI_DIR}/ DESTINATION ${WORK}/feature)
file(WRITE ${WORK}/feature/js/69-fs00test.js "Shunt.registerPage({ id: 'fs00test', title:'T', order:69, featureId:'fs00test', mount(){}, update(){} });\n")
file(WRITE ${WORK}/feature/css/69-fs00test.css ".fs00test{color:red}\n")
run_case(feature ${WORK}/feature TRUE "[0-9]+ pages")
file(READ ${WORK}/feature.html html)
string(FIND "${html}" "/* ui/js/69-fs00test.js */" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "feature module not included")
endif()

file(REMOVE_RECURSE ${WORK})
message(STATUS "ui_build_checks: all cases passed")
