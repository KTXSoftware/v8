#!/bin/bash
export PATH=`pwd`/../depot_tools:"$PATH"
gn gen out.gn/release --args="v8_enable_inspector=true is_debug=false is_official_build=true is_component_build=true"
ninja -C out.gn/release
