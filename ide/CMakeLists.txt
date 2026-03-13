# CUI Visual Designer — drag-and-drop widget design with live code generation.
# 全部跑在 单流水线上（Application::setRoot(IdeShell)）。
add_executable(vs2022
    ide_main.cpp
    design_codegen.cpp
    design_codeparser.cpp
    design_project.cpp
    design_canvas.cpp
    design_toolbox.cpp
    design_properties.cpp
    ide_controller.cpp)
target_link_libraries(vs2022 PRIVATE cui cui_dock cui_code_editor)
# ide/*.cpp 需要 "design_model.h"，ide_main.cpp 需要 "ide/*.h"。
target_include_directories(vs2022 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR})
