set PATH=..\depot_tools;%PATH%
set DEPOT_TOOLS_WIN_TOOLCHAIN=0
call gn gen out.gn/release --args="v8_enable_inspector=true is_debug=false is_official_build=true is_component_build=true target_cpu=\"x64\""
ninja -C out.gn/release
cd out.gn\release\obj
cd v8_libplatform
lib /out:v8_libplatform.lib *.obj
cd ..
cd ..\..\..