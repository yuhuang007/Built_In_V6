cmake_minimum_required(VERSION 3.16)

# The vendor Kit stays in the workspace as a separately downloaded SDK.  It is
# deliberately not copied into the application tree so it can be upgraded and
# audited independently.  Its ESP-IDF component is discovered as an extra
# component during this P4 build.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../../ConversationalAI-Embedded-Kit-2.0-main_Korvo_2/volc_conv_ai")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(p4_main)
