# The one place the build decides what "python" means.
#
# Every generator this repo runs at configure time (gen_font, gen_library,
# gen_shell, obj2cpp, game_meta) goes through PSE_PYTHON3.
#
# It exists because `cmake -E env python3` is not an interpreter, it is a
# request that PATH contain one under that exact name. On Windows PATH does
# contain one: WindowsApps\python3.exe, the Microsoft Store app execution
# alias, a stub that installs nothing, prints "Python was not found; run
# without arguments to install from the Microsoft Store", and exits non zero.
# A real Python 3 sitting in Programs\Python310 does not help, because it is
# installed as python.exe and the stub answers to python3 first. Every such
# call site failed at configure time with a message about the generator, not
# about PATH. find_package(Python3) skips those stubs by design and reports
# the interpreter's real path, so the command line carries no name lookup at
# all.
#
# Cached because include_guard(GLOBAL) means only the first directory to
# include this file runs the find, and find_package results are directory
# scoped: without the cache, the second directory to ask expands an empty
# string, which is the failure obj_model.cmake hit before it cached its own.

include_guard(GLOBAL)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(PSE_PYTHON3 ${Python3_EXECUTABLE}
    CACHE INTERNAL "python interpreter for this repo's build time generators")
