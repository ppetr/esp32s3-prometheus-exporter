# Copyright 2026 https://github.com/ppetr/esp32s3-prometheus-exporter
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# GET /, /manual.md, /openapi.yaml (DESIGN.md §11.1). The manual and its
# generated HTML form are rendered/compressed once here, at build time
# (never on-device); openapi.yaml is embedded as-is, uncompressed, since
# it's small and the pipeline below only concerns itself with the manual.

# idf_build_get_property(... PYTHON), not CMake's own find_package(Python3):
# ESP-IDF itself (project.cmake/component.cmake) always runs its own
# build-time Python scripts through this one pinned interpreter (the
# ESP-IDF-managed venv) - find_package(Python3) would instead do its own,
# unrelated search of PATH, which could resolve to a different Python
# that's missing the 'markdown' package installed below.
idf_build_get_property(SELF_DOC_PYTHON PYTHON)

set(SELF_DOC_DOCS_DIR "${CMAKE_CURRENT_LIST_DIR}/../../docs")
set(SELF_DOC_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${SELF_DOC_GEN_DIR}")

# A build-time-only dependency (host-side Markdown rendering; never shipped
# to the device) - pinned, and self-installed into the ESP-IDF Python
# environment on first use rather than requiring a separate manual setup
# step, the same way `idf.py`'s own first run installs its own Python
# dependencies into that environment.
set(SELF_DOC_MARKDOWN_VERSION "3.11.1")
execute_process(
    COMMAND "${SELF_DOC_PYTHON}" -c "import markdown; exit(0 if markdown.__version__ == '${SELF_DOC_MARKDOWN_VERSION}' else 1)"
    RESULT_VARIABLE SELF_DOC_MARKDOWN_CHECK
    OUTPUT_QUIET ERROR_QUIET
)
if(NOT SELF_DOC_MARKDOWN_CHECK EQUAL 0)
    message(STATUS "self_doc: installing pinned 'markdown==${SELF_DOC_MARKDOWN_VERSION}' into the "
                   "ESP-IDF Python environment (one-time, build-time-only; DESIGN.md §11.1)")
    execute_process(
        COMMAND "${SELF_DOC_PYTHON}" -m pip install --quiet "markdown==${SELF_DOC_MARKDOWN_VERSION}"
        RESULT_VARIABLE SELF_DOC_PIP_RESULT
    )
    if(NOT SELF_DOC_PIP_RESULT EQUAL 0)
        message(FATAL_ERROR
            "self_doc: failed to auto-install 'markdown==${SELF_DOC_MARKDOWN_VERSION}' into the ESP-IDF "
            "Python environment (no network access?). Install it manually once with:\n"
            "    ${SELF_DOC_PYTHON} -m pip install markdown==${SELF_DOC_MARKDOWN_VERSION}\n"
            "(DESIGN.md §11.1 - a build-time-only dependency, never needed on-device.)")
    endif()
endif()

# idf_component_register() must come before add_custom_command()/
# add_custom_target() below: ESP-IDF's early requirements-only pass
# (component_get_requirements.cmake) includes every component's
# CMakeLists.txt in a restricted CMake "script mode" just to collect
# REQUIRES/EMBED_FILES lists; idf_component_register() is a no-op stub
# there, but add_custom_command() genuinely isn't scriptable and errors
# ("command is not scriptable") if reached in that mode - confirmed by
# hitting exactly that error with the two calls in the opposite order.
idf_component_register(
    SRCS "self_doc.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${SELF_DOC_GEN_DIR}"
    REQUIRES esp_http_server
    PRIV_REQUIRES uzlib_inflate
    EMBED_FILES
        "${SELF_DOC_GEN_DIR}/manual.md.deflate"
        "${SELF_DOC_GEN_DIR}/manual.html.deflate"
        "${SELF_DOC_DOCS_DIR}/openapi.yaml"
)

add_custom_command(
    OUTPUT
        "${SELF_DOC_GEN_DIR}/manual.md.deflate"
        "${SELF_DOC_GEN_DIR}/manual.html.deflate"
        "${SELF_DOC_GEN_DIR}/self_doc_sizes.h"
    COMMAND "${SELF_DOC_PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/gen_assets.py"
            --manual "${SELF_DOC_DOCS_DIR}/manual.md"
            --out-dir "${SELF_DOC_GEN_DIR}"
    DEPENDS "${SELF_DOC_DOCS_DIR}/manual.md" "${CMAKE_CURRENT_LIST_DIR}/gen_assets.py"
    VERBATIM
)
# A plain add_custom_command's outputs are only built when something
# depends on them; EMBED_FILES above creates that dependency by path (the
# embed step's own custom command DEPENDS on the exact same paths), but
# add_dependencies() below makes the ordering explicit too, and gives the
# step a visible name in build logs, rather than relying solely on that
# implicit path-matching.
add_custom_target(self_doc_generate_assets DEPENDS
    "${SELF_DOC_GEN_DIR}/manual.md.deflate"
    "${SELF_DOC_GEN_DIR}/manual.html.deflate"
    "${SELF_DOC_GEN_DIR}/self_doc_sizes.h"
)
add_dependencies(${COMPONENT_LIB} self_doc_generate_assets)
