// SPDX-License-Identifier: GPL-3.0-or-later
//
// A static library needs one translation unit to hold the QML resources
// (qt_add_resources in CMakeLists.txt) and the link line that pulls the whole
// stack in; this is it. The code lives in the domain libraries.
namespace sonora::ui {}
