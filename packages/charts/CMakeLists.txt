# ── cui_charts ─────────────────────────────────────────────────────────────
# 图表控件集：数据模型（ChartGridData / LineChartData / PieChartData …）在
# include/cui_core/charts/chart.h，绘制与 build 的实体在 src/chart.cpp。
# 自绘实现，不依赖第三方绘图库 —— 只依赖 flutter 的控件层。
cui_add_library(cui_charts
        src/chart.cpp)

target_include_directories(cui_charts PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(cui_charts PUBLIC cui_wx)
