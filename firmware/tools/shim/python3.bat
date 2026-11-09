@echo off
rem python3.bat -- Windows usually only has python.exe.
rem The SDK's post_build step (project.build -> tools/byai/unwind_6byte_table.py and
rem multi_bins.py) calls "python3" to embed the DWARF CFI table; without this shim the
rem link succeeds and then make dies with:
rem     make[4]: *** [CMakeFiles\post_build.dir\build.make:70: CMakeFiles/post_build] Error 9009
rem build_windows.bat puts this directory first on PATH, so "python3" resolves here.
python %*
