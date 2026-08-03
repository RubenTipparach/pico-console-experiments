# add_obj_model(<target> <model.obj> <out_sources_var>)
#
# Converts a Wavefront .obj into a const C++ table at build time and appends the
# generated .cpp to `out_sources_var`. Models stay editable in a modeller, and
# nothing generated is ever committed.

include_guard(GLOBAL)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(PICO_STANTA_OBJ2CPP ${CMAKE_CURRENT_LIST_DIR}/../tools/obj2cpp.py
    CACHE INTERNAL "obj2cpp converter")

function(add_obj_model TARGET MODEL OUT_SOURCES)
    get_filename_component(model_path ${MODEL} ABSOLUTE
                           BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/models)
    get_filename_component(model_name ${MODEL} NAME_WE)

    set(generated_dir ${CMAKE_CURRENT_BINARY_DIR}/generated)
    set(generated_cpp ${generated_dir}/${model_name}.cpp)
    set(generated_hpp ${generated_dir}/${model_name}.hpp)

    # The .mtl sidecar is a real input: changing a colour must rebuild. It is
    # listed under DEPENDS rather than as the command's argument because
    # obj2cpp.py finds it through the model's own mtllib line.
    set(material_path "")
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/models/${model_name}.mtl)
        set(material_path ${CMAKE_CURRENT_SOURCE_DIR}/models/${model_name}.mtl)
    endif()

    # Both generated files are listed in OUTPUT. Putting the header in
    # BYPRODUCTS instead looks tidier and breaks the build: BYPRODUCTS creates
    # no ordering dependency, so a translation unit that #includes the header
    # can be compiled before the header exists.
    add_custom_command(
        OUTPUT ${generated_cpp} ${generated_hpp}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${generated_dir}
        COMMAND ${Python3_EXECUTABLE} ${PICO_STANTA_OBJ2CPP}
                ${model_path}
                --out-cpp ${generated_cpp}
                --out-hpp ${generated_hpp}
                --name ${model_name}
        DEPENDS ${model_path} ${material_path} ${PICO_STANTA_OBJ2CPP}
        COMMENT "obj2cpp ${model_name}.obj"
        VERBATIM
    )

    set(${OUT_SOURCES} ${${OUT_SOURCES}} ${generated_cpp} PARENT_SCOPE)
endfunction()
