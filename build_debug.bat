set PATH=..\depot_tools;%PATH%
set DEPOT_TOOLS_WIN_TOOLCHAIN=0
call gn gen out.gn/debug --args="v8_enable_inspector=true is_debug=true is_component_build=true target_cpu=\"x64\""
ninja -C out.gn/debug
cd out.gn\release\obj
cd v8_libbase
lib /out:v8_libbase.lib *.obj
cd ..
cd v8_libplatform
lib /out:v8_libplatform.lib *.obj
cd ..
cd ..\..\..