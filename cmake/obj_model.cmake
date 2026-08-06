# add_obj_model(<slug> <model.obj> <out_sources_var>)
#
# Converts a Wavefront .obj into a const C++ table at build time and appends the
# generated .cpp to `out_sources_var`. Models stay editable in a modeller, and
# nothing generated is ever committed.
#
# `slug` is the game the model belongs to, and it is what keeps two games from
# colliding when the console links them into one binary. It must be the game's
# own slug and not the name of the target being built: the preview harnesses
# compile the same render.cpp as the device, so a model reached through
# `<slug>_preview` would be a different namespace to the one that source says.

include_guard(GLOBAL)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# find_package results are directory scoped, and include_guard(GLOBAL) means
# only the FIRST directory to include this file runs the find. Cache the
# interpreter path so every later add_obj_model call expands a real python
# rather than an empty string. Without this, the second game to ship models
# in its tests breaks the first one's build.
set(PICO_PYTHON3 ${Python3_EXECUTABLE}
    CACHE INTERNAL "python interpreter for obj2cpp")

set(PICO_OBJ2CPP ${CMAKE_CURRENT_LIST_DIR}/../tools/obj2cpp.py
    CACHE INTERNAL "obj2cpp converter")

function(add_obj_model SLUG MODEL OUT_SOURCES)
    get_filename_component(model_path ${MODEL} ABSOLUTE
                           BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/models)
    get_filename_component(model_name ${MODEL} NAME_WE)

    # Hyphens are fine in a directory and not in an identifier, and pico-santa
    # is exactly that case. obj2cpp.py sanitizes this too; doing it here as
    # well is what keeps the include path and the namespace the same word.
    string(REGEX REPLACE "[^A-Za-z0-9_]" "_" model_ns ${SLUG})

    # One directory per game, on an include path that stops at its parent, so a
    # game includes "<game>/tree.hpp" and gets its own. Flat, every game's
    # generated directory is on the console's include path at once and
    # "tree.hpp" is whichever game the -I order happened to reach first.
    set(generated_dir ${CMAKE_CURRENT_BINARY_DIR}/generated/${model_ns})
    set(generated_cpp ${generated_dir}/${model_name}.cpp)
    set(generated_hpp ${generated_dir}/${model_name}.hpp)

    # The .mtl sidecar is a real input: changing a colour must rebuild. It is
    # listed under DEPENDS rather than as the command's argument because
    # obj2cpp.py finds it through the model's own mtllib line.
    get_filename_component(model_dir ${model_path} DIRECTORY)
    set(material_path "")
    if(EXISTS ${model_dir}/${model_name}.mtl)
        set(material_path ${model_dir}/${model_name}.mtl)
    endif()

    # Both generated files are listed in OUTPUT. Putting the header in
    # BYPRODUCTS instead looks tidier and breaks the build: BYPRODUCTS creates
    # no ordering dependency, so a translation unit that #includes the header
    # can be compiled before the header exists.
    add_custom_command(
        OUTPUT ${generated_cpp} ${generated_hpp}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${generated_dir}
        COMMAND ${PICO_PYTHON3} ${PICO_OBJ2CPP}
                ${model_path}
                --out-cpp ${generated_cpp}
                --out-hpp ${generated_hpp}
                --name ${model_name}
                --namespace ${model_ns}
        DEPENDS ${model_path} ${material_path} ${PICO_OBJ2CPP}
        COMMENT "obj2cpp ${model_name}.obj"
        VERBATIM
    )

    set(${OUT_SOURCES} ${${OUT_SOURCES}} ${generated_cpp} PARENT_SCOPE)
endfunction()
