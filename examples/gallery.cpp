/// @file gallery.cpp — CUI gallery（控件体系演示）。
///
/// 整个 gallery 是**一棵**控件树：上游控件体系（widgets/*.h）直接挂在
/// Application 的单条流水线上（Application::setRoot）。
///
/// 历史：本文件曾同时维护两套 gallery —— 旧控件体系的页面
/// （basic/input/layout/data/navigation/dialogs 等 + Ribbon/MenuBar/Sidebar 外壳）
/// 与当前控件体系的 gallery（gallery_shell.h 的壳与页面）。旧体系整体移除后，
/// 只保留当前体系，宿主桥也随之删除，改用 Application::setRoot 原生挂载。
///
/// 离屏冒烟（配合 CUI_SHOT）：
///   CUI_SHOT=<path>        启动即渲染一帧并截图
///   CUI_EXIT_AFTER_START=1 渲染一帧后退出（验证析构路径）
///   CUI_PAGE=<n>   直接打开指定页（由 gallery 壳读取）
///   CUI_DOWN="x,y" 只注入按下（把「按 Down 打开」的浮层留在屏上）
///   CUI_HOVER="x,y;x,y" 注入鼠标移动（悬停高亮 / tooltip）
///   CUI_SCROLL="dy"     注入滚轮滚动
///   CUI_CLICK="x,y;x,y" 注入点击
///   CUI_KEY="hello"     注入键盘输入
///   CUI_SEQ="click:1,2;hover:3,4" 动作序列（显式顺序，优先于上面的开关；
///            另支持 down/hover/up/click/rclick/scroll/tick/key/keycode）
///   CUI_KEYCODE="9;9"   注入特殊键（cui_core::KeyCode 枚举值：8=Up,9=Down,10=PageUp,11=PageDown）
///   CUI_THEME="dark"    启动主题（light/dark/ocean/forest/skyblue/orange/lavender/ide）
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "cui/foundation/application.h"
#include "cui/foundation/geometry.h"
#include "cui/foundation/theme.h"
#include "cui/foundation/ticker.h"
#include "cui/foundation/translator.h"
#include "cui/foundation/window.h"
#include "cui/platform/event.h"
#include "cui/framework/app_root.h"

namespace cui {

// ── i18n ─────────────────────────────────────────────────────────────────────

void loadTranslations() {
    auto& t = Translator::instance();
    // 以中文为基语言：tr(key) 在 zh 下回退为 key（中文原文），
    // 切到其它语言时由下方注册的条目翻译。无对应条目则优雅回退为原文。
    t.setLanguage("zh");
    // 启动时可经 CUI_LANG 强制初始语言（便于演示/离屏验证）：en/zh/zh-TW/fr/ko。
    if (const char* l = std::getenv("CUI_LANG"))
        t.setLanguage(l);

    // chrome 文案对照表（键即 gallery 源码中的原始字符串）。
    // 列：key, en, zh(仅当 key 本身是英文时需要), fr, ko, zh-TW。
    struct R {
        const char* k;
        const char* en;
        const char* zh;  // 非 nullptr 且与 k 不同才注册
        const char* fr;
        const char* ko;
        const char* tw;
    };
    static const R k[] = {
        // ── 菜单栏 ──
        {"文件", "File", nullptr, "Fichier", "파일", "檔案"},
        {"新建", "New", nullptr, "Nouveau", "새로 만들기", "新增"},
        {"打开", "Open", nullptr, "Ouvrir", "열기", "開啟"},
        {"保存", "Save", nullptr, "Enregistrer", "저장", "儲存"},
        {"退出", "Exit", nullptr, "Quitter", "종료", "結束"},
        {"编辑", "Edit", nullptr, "Modifier", "편집", "編輯"},
        {"撤销", "Undo", nullptr, "Annuler", "실행 취소", "復原"},
        {"重做", "Redo", nullptr, "Rétablir", "다시 실행", "重做"},
        {"剪切", "Cut", nullptr, "Couper", "잘라내기", "剪下"},
        {"复制", "Copy", nullptr, "Copier", "복사", "複製"},
        {"粘贴", "Paste", nullptr, "Coller", "붙여넣기", "貼上"},
        {"视图", "View", nullptr, "Affichage", "보기", "檢視"},
        {"折叠/展开侧栏", "Collapse/Expand Sidebar", nullptr, "Réduire/Déployer", "사이드바 접기/펼치기", "折疊/展開側欄"},
        {"上一页", "Previous Page", nullptr, "Page précédente", "이전 페이지", "上一頁"},
        {"下一页", "Next Page", nullptr, "Page suivante", "다음 페이지", "下一頁"},
        {"帮助", "Help", nullptr, "Aide", "도움말", "說明"},
        {"关于 CUI Gallery", "About CUI Gallery", nullptr, "À propos", "CUI 갤러리 정보", "關於 CUI Gallery"},
        {"主题", "Theme", nullptr, "Thème", "테마", "佈景主題"},
        {"语言", "Language", nullptr, "Langue", "언어", "語言"},
        {"液态玻璃", "Liquid Glass", nullptr, "Verre liquide", "리퀴드 글라스", "液態玻璃"},
        // ── Ribbon ──
        {"开始", "Home", nullptr, "Accueil", "홈", "開始"},
        {"导航", "Navigate", nullptr, "Naviguer", "탐색", "導覽"},
        {"跳转", "Jump", nullptr, "Aller à", "이동", "跳轉"},
        {"概览", "Overview", nullptr, "Aperçu", "개요", "概覽"},
        {"数据", "Data", nullptr, "Données", "데이터", "資料"},
        {"外壳", "Shell", nullptr, "Coque", "셸", "外殼"},
        {"窗口", "Window", nullptr, "Fenêtre", "창", "視窗"},
        {"折叠侧栏", "Collapse Sidebar", nullptr, "Réduire", "사이드바 접기", "折疊側欄"},
        {"置顶", "Always on Top", nullptr, "Toujours au-dessus", "항상 위에", "置頂"},
        {"侧栏", "Sidebar", nullptr, "Barre latérale", "사이드바", "側欄"},
        {"折叠", "Collapse", nullptr, "Réduire", "접기", "折疊"},
        {"展开", "Expand", nullptr, "Déployer", "펼치기", "展開"},
        {"页面", "Page", nullptr, "Page", "페이지", "頁面"},
        {"首页", "Home", nullptr, "Accueil", "홈", "首頁"},
        {"末页", "Last", nullptr, "Fin", "마지막", "末頁"},
        {"关于", "About", nullptr, "À propos", "정보", "關於"},
        {"文档", "Docs", nullptr, "Documentation", "문서", "文件"},
        // ── 侧边栏分组 ──
        {"入门", "Getting Started", nullptr, "Démarrage", "시작하기", "入門"},
        {"Basics", "Basics", "基础", "Bases", "기본", "基礎"},
        {"Data Display", "Data Display", "数据展示", "Affichage de données", "데이터 표시", "資料顯示"},
        {"Layout", "Layout", "布局", "Mise en page", "레이아웃", "佈局"},
        {"Navigation", "Navigation", "导航", "Navigation", "탐색", "導覽"},
        {"Feedback", "Feedback", "反馈", "Retour", "피드백", "回饋"},
        {"Advanced", "Advanced", "高级", "Avancé", "고급", "進階"},
        // ── 页面名 ──
        {"概览", "Overview", nullptr, "Aperçu", "개요", "概覽"},
        {"按钮", "Buttons", nullptr, "Boutons", "버튼", "按鈕"},
        {"选择", "Selection", nullptr, "Sélection", "선택", "選擇"},
        {"滑块", "Slider", nullptr, "Curseur", "슬라이더", "滑桿"},
        {"图标", "Icons", nullptr, "Icônes", "아이콘", "圖示"},
        {"文本", "Text", nullptr, "Texte", "텍스트", "文字"},
        {"文本输入", "Text Input", nullptr, "Saisie de texte", "텍스트 입력", "文字輸入"},
        {"输入+", "Input+", nullptr, "Saisie+", "입력+", "輸入+"},
        {"值输入", "Value Input", nullptr, "Valeur", "값 입력", "值輸入"},
        {"数据", "Data", nullptr, "Données", "데이터", "資料"},
        {"数据+", "Data+", nullptr, "Données+", "데이터+", "資料+"},
        {"组件", "Components", nullptr, "Composants", "컴포넌트", "元件"},
        {"Chips", "Chips", "标签", "Puces", "칩", "標籤"},
        {"布局", "Layout", nullptr, "Mise en page", "레이아웃", "佈局"},
        {"布局+", "Layout+", nullptr, "Mise en page+", "레이아웃+", "佈局+"},
        {"布局2", "Layout2", nullptr, "Mise en page 2", "레이아웃2", "佈局2"},
        {"滚动", "Scroll", nullptr, "Défilement", "스크롤", "捲動"},
        {"滚动+", "Scroll+", nullptr, "Défilement+", "스크롤+", "捲動+"},
        {"下拉刷新", "Pull to Refresh", nullptr, "Tirer pour actualiser", "당겨서 새로고침", "下拉重新整理"},
        {"导航", "Navigation", nullptr, "Navigation", "탐색", "導覽"},
        {"导航2", "Navigation2", nullptr, "Navigation 2", "탐색2", "導覽2"},
        {"窗口栏", "Title Bar", nullptr, "Barre de titre", "제목 표시줄", "視窗標題列"},
        {"反馈", "Feedback", nullptr, "Retour", "피드백", "回饋"},
        {"反馈2", "Feedback2", nullptr, "Retour 2", "피드백2", "回饋2"},
        {"对话框", "Dialog", nullptr, "Boîte de dialogue", "대화상자", "對話方塊"},
        {"手势", "Gestures", nullptr, "Gestes", "제스처", "手勢"},
        {"交互+", "Interaction+", nullptr, "Interaction+", "상호작용+", "互動+"},
        {"高级", "Advanced", nullptr, "Avancé", "고급", "進階"},
        {"Material", "Material", "材质", "Matériau", "머티리얼", "材質"},
        {"浮层+", "Overlay+", nullptr, "Superposition+", "오버레이+", "浮層+"},
        {"外壳", "Shell", nullptr, "Coque", "셸", "外殼"},
        // ── 窗口标题 ──
        {"CUI Gallery", "CUI Gallery", "CUI 画廊", "CUI Gallery", "CUI 갤러리", "CUI 畫廊"},
    };
    for (const auto& r : k) {
        t.add(r.k, "en", r.en);
        if (r.zh && std::string(r.zh) != r.k)
            t.add(r.k, "zh", r.zh);
        if (r.fr) t.add(r.k, "fr", r.fr);
        if (r.ko) t.add(r.k, "ko", r.ko);
        if (r.tw) t.add(r.k, "zh-TW", r.tw);
    }
    std::fprintf(stderr, "[i18n] registered %zu chrome entries\n", std::size(k));
}

namespace {

/// 解析 "x,y" 或 "x,y;x,y" 形式的坐标列表。
std::vector<Point> parsePoints(const char* s) {
    std::vector<Point> out;
    std::string buf(s ? s : "");
    size_t pos = 0;
    while (pos <= buf.size()) {
        size_t next = buf.find(';', pos);
        std::string tok = (next == std::string::npos) ? buf.substr(pos)
                                                      : buf.substr(pos, next - pos);
        double x = 0.0, y = 0.0;
        if (std::sscanf(tok.c_str(), "%lf,%lf", &x, &y) == 2)
            out.push_back(Point{x, y});
        if (next == std::string::npos)
            break;
        pos = next + 1;
    }
    return out;
}

/// 解析 "45;46" 形式的整数列表。
std::vector<int> parseInts(const char* s) {
    std::vector<int> out;
    std::string buf(s ? s : "");
    size_t pos = 0;
    while (pos <= buf.size()) {
        size_t next = buf.find(';', pos);
        std::string tok = (next == std::string::npos) ? buf.substr(pos)
                                                      : buf.substr(pos, next - pos);
        int v = 0;
        if (std::sscanf(tok.c_str(), "%d", &v) == 1)
            out.push_back(v);
        if (next == std::string::npos)
            break;
        pos = next + 1;
    }
    return out;
}

/// 注入一个指针事件到 app 的控件流水线。
void injectPointer(Application& app, EventType type, const Point& p,
                   int button = 1, double wheelY = 0) {
    PlatformEvent ev;
    ev.type = type;
    ev.pos = p;
    ev.button = button;
    ev.deltaY = wheelY;
    // 冒烟注入的滚轮量直接是像素（不走平台的「格数」换算）。
    if (type == EventType::MouseWheel)
        ev.wheelInNotches = false;
    app.dispatchInput(ev);
}

/// 推进动画并渲染一帧（离屏路径没有真实事件循环，手动泵 Ticker）。
void pumpFrame(Application& app) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Ticker::tickAllActive(Duration(0));
    // runAsync / scheduleRender 的回调排在 GUI 任务队列里，平时由事件循环的
    // Phase 2 排水；冒烟脚本不进 run()，所以每帧自己排一次 —— 否则后台线程的
    // 结果永远上不了屏（Async 页会一直停在 0/8）。
    app.runPendingGuiTasks();
    app.renderFrame();
}

/// 一个冒烟动作：kind ∈ {down, hover, up, click, rclick, scroll, tick, key, keycode}。
///
/// 序列语法（见 CUI_SEQ）："click:543,277;hover:700,340;scroll:120"。
/// 各动作的坐标/参数可含多个，用 ';' 分隔。`scroll` 另接受三点形式
/// `scroll:x,y,dy`（滚轮打在指定点，嵌套滚动条用例需要）。
struct SmokeAction {
    std::string kind;
    std::string arg;
};

/// 解析 CUI_SEQ 动作序列。
std::vector<SmokeAction> parseSequence(const char* s) {
    std::vector<SmokeAction> out;
    std::string buf(s ? s : "");
    size_t pos = 0;
    while (pos <= buf.size()) {
        size_t next = buf.find(';', pos);
        std::string tok = (next == std::string::npos) ? buf.substr(pos)
                                                      : buf.substr(pos, next - pos);
        size_t colon = tok.find(':');
        if (colon != std::string::npos)
            out.push_back(SmokeAction{tok.substr(0, colon), tok.substr(colon + 1)});
        if (next == std::string::npos)
            break;
        pos = next + 1;
    }
    return out;
}

/// 执行一个冒烟动作。
void runAction(Application& app, const SmokeAction& act) {
    if (act.kind == "down") {
        for (const auto& p : parsePoints(act.arg.c_str())) {
            std::fprintf(stderr, "[gallery] smoke down at (%.0f, %.0f)\n", p.x, p.y);
            injectPointer(app, EventType::MouseDown, p);
            pumpFrame(app);
        }
    } else if (act.kind == "hover") {
        for (const auto& p : parsePoints(act.arg.c_str())) {
            std::fprintf(stderr, "[gallery] smoke hover at (%.0f, %.0f)\n", p.x, p.y);
            injectPointer(app, EventType::MouseMove, p);
            pumpFrame(app);
        }
    } else if (act.kind == "up") {
        // 显式抬手。`click` 把 down/up 挤在同一点＝另起一次按下，会重算该 pointer
        // 的 down 命中链，所以「按下-拖动-松开」（Draggable 投放）必须用这条。
        for (const auto& p : parsePoints(act.arg.c_str())) {
            std::fprintf(stderr, "[gallery] smoke up at (%.0f, %.0f)\n", p.x, p.y);
            injectPointer(app, EventType::MouseUp, p);
            pumpFrame(app);
        }
    } else if (act.kind == "click") {
        for (const auto& p : parsePoints(act.arg.c_str())) {
            std::fprintf(stderr, "[gallery] smoke click at (%.0f, %.0f)\n", p.x, p.y);
            injectPointer(app, EventType::MouseDown, p);
            injectPointer(app, EventType::MouseUp, p);
            pumpFrame(app);
        }
    } else if (act.kind == "rclick") {
        // 右键点击（X11 语义：button=3 ⇒ kSecondaryMouseButton）。
        for (const auto& p : parsePoints(act.arg.c_str())) {
            std::fprintf(stderr, "[gallery] smoke rclick at (%.0f, %.0f)\n", p.x, p.y);
            injectPointer(app, EventType::MouseDown, p, 3);
            injectPointer(app, EventType::MouseUp, p, 3);
            pumpFrame(app);
        }
    } else if (act.kind == "scroll") {
        // 两种写法：`scroll:<dy>` 打在窗口中部（老口径，既有脚本不变），
        // `scroll:<x>,<y>,<dy>` 打在指定点 —— 嵌套滚动条必须把滚轮精确落在里层。
        double x = 0.0, y = 0.0, dy = 0.0;
        if (std::sscanf(act.arg.c_str(), "%lf,%lf,%lf", &x, &y, &dy) != 3) {
            x = 500.0;
            y = 400.0;
            if (std::sscanf(act.arg.c_str(), "%lf", &dy) != 1)
                return;
        }
        std::fprintf(stderr, "[gallery] smoke scroll dy=%.0f at (%.0f, %.0f)\n", dy, x, y);
        injectPointer(app, EventType::MouseWheel, Point{x, y}, 0, dy);
        pumpFrame(app);
    } else if (act.kind == "tick") {
        // 纯动画帧：不注入输入，只推进时间并渲染若干帧。
        // 用于复现/验证「滚动结束后的动画帧」场景（如 repaint boundary
        // 的 layerOffset_ 漂移：只有无新滚动的纯动画帧才会暴露）。
        int n = 1;
        std::sscanf(act.arg.c_str(), "%d", &n);
        for (int i = 0; i < n; ++i)
            pumpFrame(app);
    } else if (act.kind == "key") {
        std::fprintf(stderr, "[gallery] smoke type: %s\n", act.arg.c_str());
        for (char c : act.arg) {
            PlatformEvent ev;
            ev.type = EventType::KeyDown;
            ev.text = std::string(1, c);
            app.dispatchInput(ev);
        }
        pumpFrame(app);
    } else if (act.kind == "keycode") {
        for (int key : parseInts(act.arg.c_str())) {
            std::fprintf(stderr, "[gallery] smoke keycode %d\n", key);
            PlatformEvent ev;
            ev.type = EventType::KeyDown;
            ev.key = key;
            app.dispatchInput(ev);
            pumpFrame(app);
        }
    }
}

/// 运行冒烟动作：CUI_SEQ 优先（显式顺序）；否则回退到各独立开关
/// （down → hover → scroll → click → key → keycode）。
void runSmoke(Application& app) {
    if (const char* seq = std::getenv("CUI_SEQ")) {
        for (const auto& act : parseSequence(seq))
            runAction(app, act);
        return;
    }
    struct { const char* env; const char* kind; } kFallback[] = {
        {"CUI_DOWN", "down"}, {"CUI_HOVER", "hover"}, {"CUI_SCROLL", "scroll"},
        {"CUI_CLICK", "click"}, {"CUI_KEY", "key"}, {"CUI_KEYCODE", "keycode"},
    };
    for (const auto& f : kFallback) {
        if (const char* v = std::getenv(f.env))
            runAction(app, SmokeAction{f.kind, v});
    }
}

}  // namespace

}  // namespace cui_core

int main() {
    using namespace cui;
    loadTranslations();

    // 调试辅助：启动主题（离屏截图验收暗色/其他预设用）。
    if (const char* name = std::getenv("CUI_THEME"))
        theme() = Theme::fromName(name);

    Application app;
    app.createWindow(tr("CUI Gallery"), 1080, 800);

    // 原生挂载：应用根由 cui_core::createAppRoot() 注入（实现在 cui_app_root），
    // 入口本身不知道具体跑的是哪棵控件树（单一 RenderView）。
    app.setRoot(cui::createAppRoot());
    app.renderFrame();

    // 调试辅助：CUI_SHOT=<path> 时启动即保存一帧截图（raster 模式下可靠）。
    if (const char* shotPath = std::getenv("CUI_SHOT")) {
        runSmoke(app);
        app.saveScreenshot(shotPath);
        // 多窗口页：每个副窗口有自己的 surface，逐窗另存 <shot>.winN.png。
        for (size_t i = 1; i < app.windowCount(); ++i) {
            AppWindow* w = app.windowAt(i);
            if (w && w->isOpen() && w->hasRoot())
                w->saveScreenshot(std::string(shotPath) + ".win" +
                                  std::to_string(i) + ".png");
        }
    }
    // 调试辅助：CUI_EXIT_AFTER_START=1 渲染一帧后立即退出（验证析构路径）。
    if (std::getenv("CUI_EXIT_AFTER_START")) {
        app.renderFrame();
        return 0;
    }
    return app.run();
}
