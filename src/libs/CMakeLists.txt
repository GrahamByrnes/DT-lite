include_directories("${CMAKE_CURRENT_BINARY_DIR}/../")

include(manage-symbol-visibility)

add_definitions(-include common/module_api.h)
add_definitions(-include libs/lib_api.h)

add_custom_command(
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/authors_h.sh ${CMAKE_SOURCE_DIR}/AUTHORS
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/tools/darktable_authors.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/tools/
  COMMAND bash ${CMAKE_SOURCE_DIR}/tools/authors_h.sh ${CMAKE_SOURCE_DIR}/AUTHORS ${CMAKE_CURRENT_BINARY_DIR}/tools/darktable_authors.h
  COMMENT "Generating authors.h for about dialog."
)

add_custom_target(
  generate_authors_h ALL
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/authors_h.sh ${CMAKE_SOURCE_DIR}/AUTHORS ${CMAKE_CURRENT_BINARY_DIR}/tools/darktable_authors.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

# The modules
set(MODULES export import copy_history colorpicker styles tagging image select collect recentcollect metadata metadata_view module_view navigation histogram history snapshots backgroundjobs masks duplicate)

# The tools
set(MODULES ${MODULES} viewswitcher)
set(MODULES ${MODULES} darktable_label)
set(MODULES ${MODULES} ratings)
set(MODULES ${MODULES} colorlabels)
set(MODULES ${MODULES} filter)
set(MODULES ${MODULES} lighttable_mode)
set(MODULES ${MODULES} view_toolbox)
set(MODULES ${MODULES} module_toolbox)
set(MODULES ${MODULES} filmstrip)
set(MODULES ${MODULES} hinter)
set(MODULES ${MODULES} global_toolbox)
set(MODULES ${MODULES} timeline)
set(MODULES ${MODULES} image_infos)

if(BUILD_BATTERY_INDICATOR)
  set(MODULES ${MODULES} battery_indicator)
endif()

# modules
add_library(import MODULE "import.c")
add_library(export MODULE "export.c")
add_library(copy_history MODULE "copy_history.c")
add_library(styles MODULE "styles.c")
add_library(image MODULE "image.c")
add_library(tagging MODULE "tagging.c")
add_library(select MODULE "select.c")
add_library(collect MODULE "collect.c")
add_library(recentcollect MODULE "recentcollect.c")
add_library(metadata MODULE "metadata.c")
add_library(metadata_view MODULE "metadata_view.c")
add_library(navigation MODULE "navigation.c")
add_library(histogram MODULE "histogram.c")
add_library(history MODULE "history.c")
add_library(snapshots MODULE "snapshots.c")
add_library(backgroundjobs MODULE "backgroundjobs.c")
add_library(colorpicker MODULE "colorpicker.c")
add_library(masks MODULE "masks.c")
add_library(duplicate MODULE "duplicate.c")
add_library(module_view MODULE "module_view.c")

# tools
add_library(viewswitcher MODULE "tools/viewswitcher.c")

add_library(darktable_label MODULE "tools/darktable.c")
add_dependencies(darktable_label generate_authors_h)

add_library(colorlabels MODULE "tools/colorlabels.c")
add_library(ratings MODULE "tools/ratings.c")
add_library(filter MODULE "tools/filter.c")
add_library(lighttable_mode MODULE "tools/lighttable.c")
add_library(view_toolbox MODULE "tools/view_toolbox.c")
add_library(module_toolbox MODULE "tools/module_toolbox.c")
add_library(filmstrip MODULE "tools/filmstrip.c")
add_library(hinter MODULE "tools/hinter.c")
add_library(global_toolbox MODULE "tools/global_toolbox.c")
add_library(timeline MODULE "tools/timeline.c")
add_library(image_infos MODULE "tools/image_infos.c")

if(BUILD_BATTERY_INDICATOR)
  add_library(battery_indicator MODULE "tools/battery_indicator.c")
endif()

# the module specific to print mode
if(CUPS_FOUND)
  add_library(print_settings MODULE "print_settings.c")
  set(MODULES ${MODULES} print_settings)
endif(CUPS_FOUND)

# Add libs references
foreach(module ${MODULES})
	target_link_libraries(${module} lib_darktable)
endforeach(module)

# Add installation target for each module
# TODO: Modules are now not only lighttable related and should be placed
#       in a better matching structure then just in lighttable directory.
foreach(module ${MODULES})
    if (WIN32)
      _detach_debuginfo (${module} ${CMAKE_INSTALL_LIBDIR}/darktable/plugins/lighttable)
    else()
        set_target_properties(${module}
                              PROPERTIES
                              INSTALL_RPATH ${CMAKE_INSTALL_LIBDIR_RPATH})
    endif(WIN32)
    install(TARGETS  ${module} DESTINATION ${CMAKE_INSTALL_LIBDIR}/darktable/plugins/lighttable COMPONENT DTApplication)
endforeach(module)
