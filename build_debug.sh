#!/bin/bash
export PATH=`pwd`/depot_tools:"$PATH"
gn gen out.gn/debug --args="v8_enable_inspector=true is_debug=true is_component_build=true"
ninja -C out.gn/debug
cd out.gn/debug/obj/v8_libplatform
ar cr libv8_libplatform.a *.o

