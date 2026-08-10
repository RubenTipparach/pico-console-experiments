# add_texture(<slug> <picture.png> <out_sources_var>)
#
# The picture equivalent of add_obj_model: converts a PNG into a const
# pse::Texture at build time and appends the generated .cpp to
# `out_sources_var`. The art stays a real PNG that opens in any editor and
# nothing generated is committed.
#
# `slug` is the game the texture belongs to, for exactly the reason
# obj_model.cmake gives: the console links several games into one binary and a
# flat namespace would make "wall.hpp" whichever game the include order reached
# first.

include_guard(GLOBAL)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(PICO_PYTHON3 ${Python3_EXECUTABLE}
    CACHE INTERNAL "python interpreter for asset conversion")

set(PICO_PNG2CPP ${CMAKE_CURRENT_LIST_DIR}/../tools/png2cpp.py
    CACHE INTERNAL "png2cpp converter")

function(add_texture SLUG PICTURE OUT_SOURCES)
    _add_picture(${SLUG} ${PICTURE} texture generated)
    set(${OUT_SOURCES} ${${OUT_SOURCES}} ${generated} PARENT_SCOPE)
endfunction()

# add_sprite(<slug> <picture.png> <out_sources_var>)
#
# The 2D counterpart. Same pipeline, different struct on the way out: a
# pse::Sprite is any size, four bytes a pixel, and carries alpha as a mask, so
# a game's art is a real PNG rather than a pile of draw calls that only exist
# at runtime. A sheet is an ordinary sprite; the caller names the cell.
function(add_sprite SLUG PICTURE OUT_SOURCES)
    _add_picture(${SLUG} ${PICTURE} sprite generated)
    set(${OUT_SOURCES} ${${OUT_SOURCES}} ${generated} PARENT_SCOPE)
endfunction()

function(_add_picture SLUG PICTURE KIND OUT_SOURCES)
    get_filename_component(picture_path ${PICTURE} ABSOLUTE
                           BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/assets)
    get_filename_component(picture_name ${PICTURE} NAME_WE)

    string(REGEX REPLACE "[^A-Za-z0-9_]" "_" texture_ns ${SLUG})

    set(generated_dir ${CMAKE_CURRENT_BINARY_DIR}/generated/${texture_ns})
    set(generated_cpp ${generated_dir}/${picture_name}.cpp)
    set(generated_hpp ${generated_dir}/${picture_name}.hpp)

    # Header in OUTPUT and not BYPRODUCTS, same trap obj_model.cmake documents:
    # BYPRODUCTS creates no ordering dependency, so a translation unit that
    # includes the header can be compiled before the header exists.
    add_custom_command(
        OUTPUT ${generated_cpp} ${generated_hpp}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${generated_dir}
        COMMAND ${PICO_PYTHON3} ${PICO_PNG2CPP}
                ${picture_path}
                --out-cpp ${generated_cpp}
                --out-hpp ${generated_hpp}
                --name ${picture_name}
                --namespace ${texture_ns}
                --kind ${KIND}
        DEPENDS ${picture_path} ${PICO_PNG2CPP}
        COMMENT "png2cpp ${KIND} ${picture_name}.png"
        VERBATIM
    )

    set(${OUT_SOURCES} ${generated_cpp} PARENT_SCOPE)
endfunction()
