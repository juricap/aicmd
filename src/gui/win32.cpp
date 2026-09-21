#include "core/core.h"
#include "host/state.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>

using namespace ac;
namespace {
constexpr UINT TIMER = 1;
constexpr int MENU_BASE = 1000;
struct Pane {
    HWND tabs{}, path{}, pathLabel{}, list{}, summary{};
    Id tab = 0;
    std::vector<Id> tabIds;
    std::vector<std::wstring> tabLabels;
    std::shared_ptr<const Snapshot> snapshot;
    bool selectionDirty = false;
    bool editingPath = false;
    int listWidth = 0;
};
struct App {
    Core core;
    HWND window{}, status{};
    std::array<Pane,2> panes;
    HFONT font{};
    HIMAGELIST icons{};
    HBRUSH activePath{}, inactivePath{};
    UINT dpi = 96;
    bool syncing = false, saveAllowed = true, dirty = false;
    fs::path statePath, editor;
    HANDLE stateLock = INVALID_HANDLE_VALUE;
    Id seen = 0;
    ULONGLONG lastSave = 0;
    std::wstring notice;
    std::wstring renderedNotice;
    int drag = -1;
    RECT divider{};
    int width = 1200, height = 760;
    // Native smoke mode exercises real controls and saves an isolated session.
    fs::path smokePath;
    int smokeStage = 0, smokeTicks = 0;
    bool smokeFailed = false;
};
std::unique_ptr<App> app;
int px(int x) { return MulDiv(x,app->dpi,96); }
std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),nullptr,0);
    std::wstring out(n,L' '); if (n) MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),out.data(),n); return out;
}
std::wstring shown(const fs::path& p) { return wide(display_path(p)); }
std::wstring text(HWND h) { int n = GetWindowTextLengthW(h); std::wstring s(n+1,L'\0'); GetWindowTextW(h,s.data(),n+1); s.resize(n); return s; }
const Tab* current(int p) { return app->core.tab(p); }
void render();
void layout();
void save();
void run_command(Command c);
void edit_path(int p) {
    auto& v = app->panes[p];
    v.editingPath = true;
    ShowWindow(v.pathLabel,SW_HIDE); ShowWindow(v.path,SW_SHOW);
    SetWindowTextW(v.path,current(p)->path.c_str());
    SetFocus(v.path); SendMessageW(v.path,EM_SETSEL,0,-1);
}
void end_path(int p) {
    auto& v = app->panes[p]; v.editingPath = false;
    ShowWindow(v.path,SW_HIDE); ShowWindow(v.pathLabel,SW_SHOW); SetFocus(v.list);
}
void capture(int p) {
    auto& view = app->panes[p];
    if (!view.snapshot || !app->core.tab(p,view.tab)) return;
    Command c; c.action = Action::SelectionSet; c.panel = p; c.tab = view.tab; c.snapshot = view.snapshot->id;
    for (int i = ListView_GetNextItem(view.list,-1,LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(view.list,i,LVNI_SELECTED))
        if (i > 0 && static_cast<size_t>(i-1) < view.snapshot->entries.size()) c.selection.insert(view.snapshot->entries[i-1].name);
    int focus = ListView_GetNextItem(view.list,-1,LVNI_FOCUSED);
    if (focus > 0 && static_cast<size_t>(focus-1) < view.snapshot->entries.size()) c.focus = view.snapshot->entries[focus-1].name;
    app->core.execute(c);
    c.action = Action::ViewportSet;
    int top = ListView_GetTopIndex(view.list);
    if (top > 0 && static_cast<size_t>(top-1) < view.snapshot->entries.size()) c.anchor = view.snapshot->entries[top-1].name;
    app->core.execute(c); view.selectionDirty = false;
}
void activate(int p) { Command c; c.action = Action::PanelActivate; c.panel = p; app->core.execute(c); }
std::wstring quote_arg(const std::wstring& s) {
    std::wstring out = L"\""; size_t slashes = 0;
    for (wchar_t c : s) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'\"') out.append(slashes*2+1,L'\\'); else out.append(slashes,L'\\');
        slashes = 0; out += c;
    }
    out.append(slashes*2,L'\\'); out += L'\"'; return out;
}
void launch_editor(const fs::path& file) {
    for (wchar_t c : file.native()) if (c < 32) { app->notice = L"jptxt cannot safely receive control characters in paths yet."; return; }
    fs::path executable = app->editor;
    std::error_code ec;
    if (executable.empty()) {
        wchar_t module[32768]{}; GetModuleFileNameW(nullptr,module,32768);
        auto dir = fs::path(module).parent_path();
        std::vector<fs::path> candidates = {dir/L"jptxt.exe",dir.parent_path()/L"jptxt.exe",dir.parent_path().parent_path()/L"jptxt/build/jptxt.exe"};
        if (auto local = _wgetenv(L"LOCALAPPDATA")) candidates.push_back(fs::path(local)/L"Programs/jptxt/jptxt.exe");
        for (auto& candidate : candidates) if (fs::is_regular_file(candidate,ec)) { executable = candidate; break; }
        if (executable.empty()) { wchar_t found[32768]{}; if (SearchPathW(nullptr,L"jptxt.exe",nullptr,32768,found,nullptr)) executable = found; }
    }
    if (executable.empty()) { app->notice = L"jptxt not found. Set AICMD_EDITOR or start with --editor <jptxt.exe>."; return; }
    std::wstring args = quote_arg(executable.native())+L" "+quote_arg(file.native());
    STARTUPINFOW si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
    if (!CreateProcessW(executable.c_str(),args.data(),nullptr,nullptr,FALSE,0,nullptr,file.parent_path().c_str(),&si,&pi)) {
        app->notice = L"Cannot launch jptxt (Windows error " + std::to_wstring(GetLastError()) + L")."; return;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    app->notice = L"Opened in jptxt. Ctrl+R refreshes the listing.";
}
void run_command(Command c) {
    auto r = app->core.execute(c);
    if (r.error) app->notice = wide(r.error.message);
    else { app->notice.clear(); if (!r.launchPath.empty()) launch_editor(r.launchPath); }
    if (r.save) save();
    if (app->core.state().quit) PostMessageW(app->window,WM_CLOSE,0,0);
    app->dirty = true;
}
void action(Action a, int p = -1) {
    if (p < 0) p = app->core.state().activePanel;
    capture(p);
    Command c; c.action = a; c.panel = p;
    if (const auto* t = current(p)) {
        c.tab = t->id; c.options = t->options;
        if (a == Action::FilterSet) c.options.hidden = !c.options.hidden;
        if (a == Action::FileEdit || a == Action::FileView) { c.path = t->focus; c.snapshot = t->snapshot ? t->snapshot->id : 0; }
        if (a == Action::PanelConfigure) { auto& panel = app->core.state().panels[p]; c.flag = !panel.treeVisible; c.value = panel.treeWidth; }
    }
    run_command(c);
}
void open_item(int p) {
    capture(p); const auto& v = app->panes[p];
    int row = ListView_GetNextItem(v.list,-1,LVNI_FOCUSED);
    if (row == 0) { action(Action::Parent,p); return; }
    if (!v.snapshot || row < 1 || static_cast<size_t>(row-1) >= v.snapshot->entries.size()) return;
    const auto& e = v.snapshot->entries[row-1];
    Command c; c.panel = p; c.tab = v.tab; c.snapshot = v.snapshot->id;
    c.action = e.directory ? Action::Navigate : Action::FileEdit;
    c.path = e.directory ? v.snapshot->path / e.name : e.name;
    run_command(c);
}
void go(int p) { capture(p); Command c; c.action = Action::Navigate; c.panel = p; c.path = text(app->panes[p].path); run_command(c); end_path(p); }
HWND control(const wchar_t* cls, const wchar_t* title, DWORD style, int id) {
    HWND h = CreateWindowExW(0,cls,title,WS_CHILD|WS_VISIBLE|style,0,0,0,0,app->window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(app->font),TRUE); return h;
}
void font() {
    if (app->font) DeleteObject(app->font);
    app->font = CreateFontW(-px(12),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Tahoma");
    if (app->window) EnumChildWindows(app->window,[](HWND h,LPARAM f)->BOOL {SendMessageW(h,WM_SETFONT,f,TRUE); return TRUE;},reinterpret_cast<LPARAM>(app->font));
}
void create_controls() {
    app->dpi = GetDpiForWindow(app->window); font();
    app->status = control(L"STATIC",L"Ready",SS_LEFTNOWORDWRAP,50);
    app->activePath = CreateSolidBrush(RGB(158,185,211));
    app->inactivePath = CreateSolidBrush(RGB(209,219,228));
    app->icons = ImageList_Create(px(14),px(14),ILC_COLOR32|ILC_MASK,3,0);
    for (SHSTOCKICONID id : {SIID_FOLDER,SIID_DOCNOASSOC,SIID_FOLDEROPEN}) {
        SHSTOCKICONINFO info{}; info.cbSize = sizeof(info);
        if (SUCCEEDED(SHGetStockIconInfo(id,SHGSI_ICON|SHGSI_SMALLICON,&info))) { ImageList_AddIcon(app->icons,info.hIcon); DestroyIcon(info.hIcon); }
    }
    for (int p = 0; p < 2; ++p) {
        auto& v = app->panes[p]; int base = 100+p*100;
        v.tabs = control(WC_TABCONTROLW,L"Folder tabs",WS_TABSTOP|TCS_TOOLTIPS|TCS_FOCUSONBUTTONDOWN,base+1);
        TabCtrl_SetPadding(v.tabs,px(7),px(1));
        v.pathLabel = control(L"STATIC",L"",SS_PATHELLIPSIS|SS_NOTIFY|SS_LEFTNOWORDWRAP,base);
        v.path = control(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,base+2);
        SendMessageW(v.path,EM_SETLIMITTEXT,32760,0);
        ShowWindow(v.path,SW_HIDE);
        v.summary = control(L"STATIC",L"",SS_LEFTNOWORDWRAP,base+3);
        v.list = control(WC_LISTVIEWW,L"Files",WS_TABSTOP|LVS_REPORT|LVS_OWNERDATA|LVS_SHOWSELALWAYS|LVS_SHAREIMAGELISTS,base+4);
        ListView_SetExtendedListViewStyle(v.list,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_LABELTIP);
        SetWindowTheme(v.list,L"",L"");
        ListView_SetImageList(v.list,app->icons,LVSIL_SMALL);
        const wchar_t* names[] = {L"Name",L"Ext",L"Size",L"Date",L"Attr"}; int widths[] = {235,45,85,112,40};
        for (int i = 0; i < 5; ++i) { LVCOLUMNW col{}; col.mask = LVCF_TEXT|LVCF_WIDTH|LVCF_FMT; col.pszText = const_cast<LPWSTR>(names[i]); col.cx = px(widths[i]); col.fmt = i == 2 ? LVCFMT_RIGHT : LVCFMT_LEFT; ListView_InsertColumn(v.list,i,&col); }
    }
}
void menu() {
    HMENU bar = CreateMenu();
    auto add = [](HMENU m,Action a,const wchar_t* label){AppendMenuW(m,MF_STRING,MENU_BASE+static_cast<UINT>(a),label);};
    HMENU file = CreatePopupMenu(); add(file,Action::FileEdit,L"&Edit in jptxt\tF4");
    add(file,Action::FileView,L"Read-only &viewer (unavailable)\tF3"); EnableMenuItem(file,MENU_BASE+static_cast<UINT>(Action::FileView),MF_GRAYED);
    AppendMenuW(file,MF_SEPARATOR,0,nullptr); add(file,Action::SessionSave,L"&Save session"); add(file,Action::AppQuit,L"E&xit");
    HMENU view = CreatePopupMenu(); add(view,Action::Refresh,L"&Refresh / Retry\tCtrl+R"); add(view,Action::FilterSet,L"Show &hidden files\tCtrl+H");
    HMENU tabs = CreatePopupMenu(); add(tabs,Action::TabCreate,L"&New tab\tCtrl+T"); add(tabs,Action::TabClose,L"&Close tab\tCtrl+W"); add(tabs,Action::TabCycle,L"Ne&xt tab\tCtrl+Tab");
    AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(file),L"&File"); AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(view),L"&View"); AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(tabs),L"&Tabs");
    SetMenu(app->window,bar);
}
void layout() {
    RECT r{}; GetClientRect(app->window,&r);
    int bottom = r.bottom-px(20), gap = px(4);
    MoveWindow(app->status,px(3),bottom+px(2),r.right-px(6),px(18),TRUE);
    int split = MulDiv(r.right,app->core.state().splitPercent,100);
    app->divider = {split-gap/2,0,split+gap/2,bottom};
    for (int p = 0; p < 2; ++p) {
        auto& v = app->panes[p];
        int x = p ? split+gap/2 : px(1), width = p ? r.right-x-px(1) : split-gap/2-x;
        MoveWindow(v.tabs,x,0,width,px(23),TRUE);
        MoveWindow(v.pathLabel,x,px(23),width,px(18),TRUE);
        MoveWindow(v.path,x,px(23),width,px(18),TRUE);
        MoveWindow(v.list,x,px(41),width,std::max(20,bottom-px(59)),TRUE);
        MoveWindow(v.summary,x,bottom-px(18),width,px(18),TRUE);
        if (v.listWidth != width) {
            v.listWidth = width;
            int columns[] = {std::max(px(100),width-px(45+85+112+40)-GetSystemMetrics(SM_CXVSCROLL)-px(2)),px(45),px(85),px(112),px(40)};
            for (int i = 0; i < 5; ++i) ListView_SetColumnWidth(v.list,i,columns[i]);
        }
    }
    InvalidateRect(app->window,nullptr,FALSE);
}
void render() {
    app->syncing = true;
    for (int p = 0; p < 2; ++p) {
        auto& v = app->panes[p]; const Tab* t = current(p); if (!t) continue;
        bool switched = v.tab != t->id;
        const auto& panel = app->core.state().panels[p];
        std::vector<Id> ids; std::vector<std::wstring> labels;
        for (const auto& tab : panel.tabs) {
            ids.push_back(tab.id); auto label = shown(tab.path.filename().empty() ? tab.path : tab.path.filename());
            if (tab.loading) label += L" ..."; if (tab.error) label += L" !"; labels.push_back(label);
        }
        if (ids != v.tabIds || labels != v.tabLabels) {
            SendMessageW(v.tabs,WM_SETREDRAW,FALSE,0); TabCtrl_DeleteAllItems(v.tabs);
            for (size_t i = 0; i < ids.size(); ++i) { TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = labels[i].data(); TabCtrl_InsertItem(v.tabs,static_cast<int>(i),&item); }
            v.tabIds = ids; v.tabLabels = labels;
            SendMessageW(v.tabs,WM_SETREDRAW,TRUE,0); InvalidateRect(v.tabs,nullptr,TRUE);
        }
        auto selected = std::find(ids.begin(),ids.end(),t->id); TabCtrl_SetCurSel(v.tabs,static_cast<int>(selected-ids.begin()));
        if (switched || GetFocus() != v.path) {
            auto path = t->loading ? t->requested : t->path;
            auto native = path.native(); if (text(v.path) != native) SetWindowTextW(v.path,native.c_str());
        }
        auto bar = L" " + shown(t->loading ? t->requested : t->path);
        if (t->loading) bar += L"  [loading]";
        SetWindowTextW(v.pathLabel,bar.c_str());
        InvalidateRect(v.pathLabel,nullptr,TRUE);
        if (switched || v.snapshot != t->snapshot) {
            v.tab = t->id; v.snapshot = t->snapshot;
            SendMessageW(v.list,WM_SETREDRAW,FALSE,0);
            ListView_SetItemState(v.list,-1,0,LVIS_SELECTED|LVIS_FOCUSED);
            int count = v.snapshot ? static_cast<int>(v.snapshot->entries.size())+1 : 0;
            ListView_SetItemCountEx(v.list,count,LVSICF_NOINVALIDATEALL|LVSICF_NOSCROLL);
            int focus = 0, anchor = 0;
            if (v.snapshot) for (size_t i = 0; i < v.snapshot->entries.size(); ++i) {
                const auto& e = v.snapshot->entries[i];
                if (t->selection.count(e.name)) ListView_SetItemState(v.list,static_cast<int>(i)+1,LVIS_SELECTED,LVIS_SELECTED);
                if (t->focus == e.name) focus = static_cast<int>(i)+1;
                if (t->anchor == e.name) anchor = static_cast<int>(i)+1;
            }
            ListView_SetItemState(v.list,focus,LVIS_FOCUSED,LVIS_FOCUSED);
            ListView_EnsureVisible(v.list,0,FALSE);
            if (anchor) {
                ListView_EnsureVisible(v.list,anchor,FALSE);
                RECT row{}; ListView_GetItemRect(v.list,anchor,&row,LVIR_BOUNDS);
                ListView_Scroll(v.list,0,(anchor-ListView_GetTopIndex(v.list))*(row.bottom-row.top));
            } else ListView_EnsureVisible(v.list,focus,FALSE);
            SendMessageW(v.list,WM_SETREDRAW,TRUE,0); InvalidateRect(v.list,nullptr,TRUE);
        }
        size_t files = 0, dirs = 0;
        if (t->snapshot) for (const auto& e : t->snapshot->entries) { if (e.directory) ++dirs; else ++files; }
        std::wstring summary = std::to_wstring(t->selection.size())+L" selected / "+std::to_wstring(files)+L" file(s), "+std::to_wstring(dirs)+L" dir(s)";
        if (t->error) summary = wide(t->error.message)+L"  |  Ctrl+R Retry";
        SetWindowTextW(v.summary,summary.c_str());
        InvalidateRect(v.list,nullptr,FALSE);
        HWND header = ListView_GetHeader(v.list);
        for (int i = 0; i < 5; ++i) {
            HDITEMW item{}; item.mask = HDI_FORMAT; Header_GetItem(header,i,&item);
            item.fmt &= ~(HDF_SORTUP|HDF_SORTDOWN);
            if (i == static_cast<int>(t->options.sort)) item.fmt |= t->options.descending ? HDF_SORTDOWN : HDF_SORTUP;
            Header_SetItem(header,i,&item);
        }
    }
    const auto* t = current(app->core.state().activePanel);
    std::wstring status = app->notice;
    if (status.empty() && t && t->error) status = wide(t->error.message)+L"   |   Ctrl+R Retry";
    if (status.empty()) status = L"F4 Edit     Tab Panel     Insert Mark     Ctrl+T New tab     Ctrl+W Close tab     Ctrl+L Path     Ctrl+R Refresh     Alt+F4 Exit";
    SetWindowTextW(app->status,status.c_str());
    CheckMenuItem(GetMenu(app->window),MENU_BASE+static_cast<UINT>(Action::FilterSet),MF_BYCOMMAND|(t && t->options.hidden ? MF_CHECKED : MF_UNCHECKED));
    app->syncing = false; app->seen = app->core.state().revision; app->renderedNotice = app->notice;
    layout();
}
std::wstring cell(const Entry& e,int column) {
    if (column == 0) return e.directory ? L"["+shown(e.name)+L"]" : shown(e.name.has_extension() ? e.name.stem() : e.name);
    if (column == 1) { auto ext = shown(e.name.extension()); return e.directory || ext.empty() ? L"" : ext.substr(1); }
    if (column == 2) {
        if (e.directory) return L"<DIR>";
        if (!e.size) return L"?";
        auto n = std::to_wstring(*e.size); for (int i = static_cast<int>(n.size())-3; i > 0; i -= 3) n.insert(i,L","); return n;
    }
    if (column == 4) { std::wstring attr; attr += e.readOnly ? L'r' : L'-'; attr += e.archive ? L'a' : L'-'; attr += e.hidden ? L'h' : L'-'; attr += e.system ? L's' : L'-'; if (e.link) attr += L'l'; return attr; }
    if (!e.modified) return L"";
    time_t ts = static_cast<time_t>(*e.modified); tm local{};
    if (localtime_s(&local,&ts)) return L"";
    wchar_t out[40]{}; wcsftime(out,40,L"%Y-%m-%d %H:%M",&local); return out;
}
LRESULT notify(NMHDR* hdr) {
    for (int p = 0; p < 2; ++p) {
        auto& v = app->panes[p];
        if (hdr->hwndFrom == v.list) {
            if (hdr->code == LVN_GETDISPINFOW) {
                auto* info = reinterpret_cast<NMLVDISPINFOW*>(hdr);
                if (info->item.mask & LVIF_TEXT) {
                    std::wstring s;
                    if (info->item.iItem == 0 && info->item.iSubItem == 0) s = L"[..]";
                    else if (v.snapshot && info->item.iItem > 0 && static_cast<size_t>(info->item.iItem-1) < v.snapshot->entries.size()) s = cell(v.snapshot->entries[info->item.iItem-1],info->item.iSubItem);
                    if (info->item.pszText && info->item.cchTextMax > 0) wcsncpy_s(info->item.pszText,info->item.cchTextMax,s.c_str(),_TRUNCATE);
                }
                if (info->item.mask & LVIF_IMAGE) {
                    info->item.iImage = 2;
                    if (v.snapshot && info->item.iItem > 0 && static_cast<size_t>(info->item.iItem-1) < v.snapshot->entries.size()) info->item.iImage = v.snapshot->entries[info->item.iItem-1].directory ? 0 : 1;
                }
            } else if (hdr->code == NM_CUSTOMDRAW) {
                auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(hdr);
                if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                    int row = static_cast<int>(draw->nmcd.dwItemSpec);
                    bool focused = (ListView_GetItemState(v.list,row,LVIS_FOCUSED)&LVIS_FOCUSED) && p == app->core.state().activePanel;
                    bool marked = (ListView_GetItemState(v.list,row,LVIS_SELECTED)&LVIS_SELECTED) != 0;
                    draw->clrTextBk = focused ? RGB(0,29,46) : GetSysColor(COLOR_WINDOW);
                    draw->clrText = focused ? (marked ? RGB(255,200,120) : RGB(255,255,255)) : marked ? RGB(180,0,0) : GetSysColor(COLOR_WINDOWTEXT);
                    return CDRF_NEWFONT;
                }
            } else if (hdr->code == LVN_ODFINDITEMW) {
                auto* find = reinterpret_cast<NMLVFINDITEMW*>(hdr);
                if (v.snapshot && find->lvfi.psz) {
                    size_t count = v.snapshot->entries.size();
                    for (size_t i = 0; i < count; ++i) {
                        size_t idx = (static_cast<size_t>(std::max(0,find->iStart-1))+i) % count;
                        auto s = v.snapshot->entries[idx].name.native();
                        if (!_wcsnicmp(s.c_str(),find->lvfi.psz,wcslen(find->lvfi.psz))) return static_cast<LRESULT>(idx+1);
                    }
                }
                return -1;
            } else if (!app->syncing) {
                if (hdr->code == NM_SETFOCUS) activate(p);
                if (hdr->code == NM_DBLCLK) open_item(p);
                if (hdr->code == LVN_ITEMCHANGED || hdr->code == LVN_ODSTATECHANGED) v.selectionDirty = true;
                if (hdr->code == LVN_COLUMNCLICK) {
                    if (reinterpret_cast<NMLISTVIEW*>(hdr)->iSubItem >= 4) return 0;
                    capture(p); Command c; c.action = Action::SortSet; c.panel = p; c.options = current(p)->options;
                    auto column = static_cast<Sort>(reinterpret_cast<NMLISTVIEW*>(hdr)->iSubItem);
                    c.options.descending = c.options.sort == column ? !c.options.descending : false; c.options.sort = column; run_command(c);
                }
            }
        } else if (!app->syncing && hdr->hwndFrom == v.tabs && hdr->code == TCN_SELCHANGE) {
            capture(p); int index = TabCtrl_GetCurSel(v.tabs);
            if (index >= 0 && static_cast<size_t>(index) < v.tabIds.size()) { Command c; c.action = Action::TabActivate; c.panel = p; c.tab = v.tabIds[index]; run_command(c); activate(p); }
        }
    }
    return 0;
}
bool key(MSG& msg,int testModifiers = -1) {
    if (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN) return false;
    bool ctrl = (GetKeyState(VK_CONTROL)&0x8000) != 0, shift = (GetKeyState(VK_SHIFT)&0x8000) != 0, alt = (GetKeyState(VK_MENU)&0x8000) != 0;
    if (testModifiers >= 0) { ctrl = (testModifiers&1) != 0; shift = (testModifiers&2) != 0; alt = (testModifiers&4) != 0; }
    int p = app->core.state().activePanel; HWND focus = GetFocus();
    for (int i = 0; i < 2; ++i) { auto& v = app->panes[i]; if (focus == v.path || focus == v.tabs || focus == v.list) p = i; }
    auto& v = app->panes[p];
    if (ctrl && msg.wParam == 'L') { activate(p); edit_path(p); return true; }
    if (ctrl && msg.wParam == 'T') { action(Action::TabCreate,p); return true; }
    if (ctrl && msg.wParam == 'W') { action(Action::TabClose,p); return true; }
    if (ctrl && msg.wParam == 'R') { action(Action::Refresh,p); return true; }
    if (ctrl && msg.wParam == 'H') { action(Action::FilterSet,p); return true; }
    if (ctrl && msg.wParam == VK_TAB) { capture(p); Command c; c.action = Action::TabCycle; c.panel = p; c.value = shift ? -1 : 1; run_command(c); return true; }
    if (ctrl && msg.wParam == VK_F6) {
        if (focus == v.list) edit_path(p);
        else if (focus == v.path) { end_path(p); SetFocus(v.tabs); }
        else SetFocus(v.list);
        return true;
    }
    if (msg.wParam == VK_TAB && !ctrl) { capture(p); if (focus == v.path) end_path(p); else { activate(1-p); SetFocus(app->panes[1-p].list); } return true; }
    if (alt && msg.wParam == VK_LEFT) { action(Action::Back,p); return true; }
    if (alt && msg.wParam == VK_RIGHT) { action(Action::Forward,p); return true; }
    if (msg.wParam == VK_RETURN) { if (focus == v.path) { go(p); return true; } if (focus == v.list) { open_item(p); return true; } }
    if (msg.wParam == VK_BACK && focus == v.list) { action(Action::Parent,p); return true; }
    if (focus == v.list && (msg.wParam == VK_INSERT || msg.wParam == VK_SPACE)) {
        int row = ListView_GetNextItem(v.list,-1,LVNI_FOCUSED);
        if (row > 0) { bool selected = (ListView_GetItemState(v.list,row,LVIS_SELECTED)&LVIS_SELECTED) != 0; ListView_SetItemState(v.list,row,selected ? 0 : LVIS_SELECTED,LVIS_SELECTED); }
        if (msg.wParam == VK_INSERT && row+1 < ListView_GetItemCount(v.list)) {
            ListView_SetItemState(v.list,row+1,LVIS_FOCUSED,LVIS_FOCUSED); ListView_EnsureVisible(v.list,row+1,FALSE);
        }
        v.selectionDirty = true; InvalidateRect(v.list,nullptr,FALSE); return true;
    }
    if (focus == v.list && !ctrl && !alt && (msg.wParam == VK_UP || msg.wParam == VK_DOWN || msg.wParam == VK_HOME || msg.wParam == VK_END || msg.wParam == VK_PRIOR || msg.wParam == VK_NEXT)) {
        int row = ListView_GetNextItem(v.list,-1,LVNI_FOCUSED), count = ListView_GetItemCount(v.list);
        if (!count) return true;
        int next = row;
        if (msg.wParam == VK_UP) --next; if (msg.wParam == VK_DOWN) ++next;
        if (msg.wParam == VK_HOME) next = 0; if (msg.wParam == VK_END) next = count-1;
        if (msg.wParam == VK_PRIOR) next -= ListView_GetCountPerPage(v.list); if (msg.wParam == VK_NEXT) next += ListView_GetCountPerPage(v.list);
        next = std::clamp(next,0,count-1);
        if (shift && row > 0) ListView_SetItemState(v.list,row,LVIS_SELECTED,LVIS_SELECTED);
        ListView_SetItemState(v.list,next,LVIS_FOCUSED,LVIS_FOCUSED); ListView_EnsureVisible(v.list,next,FALSE);
        v.selectionDirty = true; InvalidateRect(v.list,nullptr,FALSE); return true;
    }
    if (ctrl && msg.wParam == 'A' && focus == v.list) { ListView_SetItemState(v.list,-1,LVIS_SELECTED,LVIS_SELECTED); ListView_SetItemState(v.list,0,0,LVIS_SELECTED); v.selectionDirty = true; return true; }
    if (msg.wParam == VK_F4 && !alt) { action(Action::FileEdit,p); return true; }
    if (msg.wParam == VK_F3) { action(Action::FileView,p); return true; }
    if (msg.wParam == VK_ESCAPE) { if (focus == v.path) end_path(p); else action(Action::JobCancel,p); return true; }
    return false;
}
void save() {
    if (!app->saveAllowed) return;
    auto state = app->core.save();
    WINDOWPLACEMENT placement{}; placement.length = sizeof(placement);
    GetWindowPlacement(app->window,&placement);
    state.width = MulDiv(placement.rcNormalPosition.right-placement.rcNormalPosition.left,96,app->dpi);
    state.height = MulDiv(placement.rcNormalPosition.bottom-placement.rcNormalPosition.top,96,app->dpi);
    auto e = save_state(app->statePath,state);
    if (e) app->notice = wide(e.message);
    app->dirty = false; app->lastSave = GetTickCount64();
}
void smoke() {
    if (app->smokePath.empty()) return;
    if (++app->smokeTicks > 600) { app->smokeFailed = true; PostMessageW(app->window,WM_CLOSE,0,0); return; }
    if (app->core.busy()) return;
    auto check = [](bool okay) { if (!okay) app->smokeFailed = true; };
    auto press = [](WPARAM code,int mods = 0) { MSG event{}; event.message = WM_KEYDOWN; event.wParam = code; key(event,mods); };
    if (app->smokeStage == 0) {
        check(ListView_GetItemCount(app->panes[0].list) > 0);
        RECT client{}, list{}; GetClientRect(app->window,&client); GetClientRect(app->panes[0].list,&list);
        check(list.right > client.right/2-px(10)); // Full width; no sidebar.
        check(list.bottom > client.bottom-px(90)); // Compact chrome.
        SendMessageW(app->window,WM_COMMAND,MENU_BASE+static_cast<UINT>(Action::TabCreate),0);
    } else if (app->smokeStage == 1) {
        check(app->core.state().panels[0].tabs.size() == 2);
        action(Action::Parent,0);
    } else if (app->smokeStage == 2) {
        check(current(0)->path == fs::current_path().parent_path());
        action(Action::TabClose,0);
    } else if (app->smokeStage == 3) {
        check(app->core.state().panels[0].tabs.size() == 1);
        check(current(0)->path == fs::current_path());
        check(current(1)->path == fs::current_path());
        SetFocus(app->panes[0].list); press(VK_TAB); press('T',1);
    } else if (app->smokeStage == 4) {
        check(app->core.state().activePanel == 1);
        check(app->core.state().panels[1].tabs.size() == 2);
        press('L',1); check(app->panes[1].editingPath);
        SetWindowTextW(app->panes[1].path,fs::current_path().parent_path().c_str()); press(VK_RETURN);
    } else if (app->smokeStage == 5) {
        check(current(1)->path == fs::current_path().parent_path());
        check(current(0)->path == fs::current_path());
        press('W',1);
    } else if (app->smokeStage == 6) {
        check(app->core.state().panels[1].tabs.size() == 1);
        check(current(1)->path == fs::current_path());
        HWND list = app->panes[1].list; SetFocus(list);
        ListView_SetItemState(list,-1,0,LVIS_SELECTED|LVIS_FOCUSED);
        ListView_SetItemState(list,1,LVIS_FOCUSED,LVIS_FOCUSED);
        press(VK_INSERT); press(VK_DOWN);
        check(ListView_GetSelectedCount(list) == 1);
        check(ListView_GetNextItem(list,-1,LVNI_FOCUSED) == 3);
        capture(1); check(current(1)->selection.size() == 1);
        press('L',1); press(VK_ESCAPE); check(!app->panes[1].editingPath);
        save();
        auto persisted = load_state(app->statePath);
        check(persisted.found && !persisted.error && persisted.state.panels[0].tabs.size() == 1);
        PostMessageW(app->window,WM_CLOSE,0,0);
    }
    ++app->smokeStage;
}
LRESULT CALLBACK window_proc(HWND hwnd,UINT message,WPARAM w,LPARAM l) {
    switch (message) {
    case WM_CREATE:
        app->window = hwnd; create_controls(); menu(); SetTimer(hwnd,TIMER,30,nullptr); return 0;
    case WM_NOTIFY: return notify(reinterpret_cast<NMHDR*>(l));
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id >= MENU_BASE && id < MENU_BASE+static_cast<int>(Action::AppQuit)+1) { action(static_cast<Action>(id-MENU_BASE)); return 0; }
        for (int p = 0; p < 2; ++p) {
            if (id == 100+p*100 && HIWORD(w) == STN_CLICKED) { activate(p); edit_path(p); }
            if (id == 102+p*100 && HIWORD(w) == EN_SETFOCUS) activate(p);
        }
        return 0;
    }
    case WM_TIMER:
        for (int p = 0; p < 2; ++p) if (app->panes[p].selectionDirty) capture(p);
        if (app->core.poll()) app->dirty = true;
        if (app->seen != app->core.state().revision || app->notice != app->renderedNotice) render();
        if (app->dirty && !app->core.busy() && GetTickCount64()-app->lastSave > 3000) save();
        smoke(); return 0;
    case WM_SIZE: if (app->status) { layout(); app->dirty = true; } return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(w); HWND control = reinterpret_cast<HWND>(l);
        for (int p = 0; p < 2; ++p) if (control == app->panes[p].pathLabel) {
            bool active = p == app->core.state().activePanel;
            SetTextColor(dc,RGB(0,0,0)); SetBkColor(dc,active ? RGB(158,185,211) : RGB(209,219,228));
            return reinterpret_cast<LRESULT>(active ? app->activePath : app->inactivePath);
        }
        SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT)); SetBkColor(dc,GetSysColor(COLOR_BTNFACE));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_GETMINMAXINFO: reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize = {px(800),px(420)}; return 0;
    case WM_DPICHANGED: {
        app->dpi = HIWORD(w); font(); auto* r = reinterpret_cast<RECT*>(l);
        SetWindowPos(hwnd,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE); return 0;
    }
    case WM_LBUTTONDOWN: {
        POINT pt{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
        if (PtInRect(&app->divider,pt)) app->drag = 2;
        if (app->drag >= 0) SetCapture(hwnd); return 0;
    }
    case WM_MOUSEMOVE:
        if (app->drag >= 0) {
            RECT r{}; GetClientRect(hwnd,&r); int p = app->core.state().activePanel;
            const auto& panel = app->core.state().panels[p]; Command c; c.action = Action::PanelConfigure; c.panel = p; c.flag = panel.treeVisible; c.value = panel.treeWidth;
            c.node = std::clamp(MulDiv(GET_X_LPARAM(l),100,std::max(1L,r.right)),20,80);
            run_command(c); layout();
        }
        return 0;
    case WM_LBUTTONUP: if (app->drag >= 0) { app->drag = -1; ReleaseCapture(); } return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) { POINT pt{}; GetCursorPos(&pt); ScreenToClient(hwnd,&pt);
            if (PtInRect(&app->divider,pt)) { SetCursor(LoadCursorW(nullptr,IDC_SIZEWE)); return TRUE; }
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(w) != WA_INACTIVE && app->seen) {
            for (int p = 0; p < 2; ++p) if (current(p) && !current(p)->loading) action(Action::Refresh,p);
        }
        break;
    case WM_CLOSE:
        capture(0); capture(1); save(); DestroyWindow(hwnd); return 0;
    case WM_DESTROY: KillTimer(hwnd,TIMER); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd,message,w,l);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show) {
    try {
        app = std::make_unique<App>();
        app->statePath = default_state_path();
        if (auto e = _wgetenv(L"AICMD_EDITOR")) app->editor = e;
        int argc = 0; auto argv = CommandLineToArgvW(GetCommandLineW(),&argc);
        fs::path left = fs::current_path(), right = left; bool explicitPaths = false;
        for (int i = 1; i < argc; ++i) {
            std::wstring arg = argv[i];
            if ((arg == L"--session" || arg == L"--editor" || arg == L"--smoke") && i+1 < argc) {
                fs::path value = absolute_path(argv[++i],fs::current_path());
                if (arg == L"--session") app->statePath = value;
                if (arg == L"--editor") app->editor = value;
                if (arg == L"--smoke") { app->smokePath = value; app->statePath = value; app->statePath += L".session.json"; explicitPaths = true; }
            } else if (!arg.empty() && arg[0] != L'-') { if (!explicitPaths) left = right = absolute_path(arg,fs::current_path()); else right = absolute_path(arg,fs::current_path()); explicitPaths = true; }
            else throw std::runtime_error("Usage: aicmd [left-directory] [right-directory] [--session file] [--editor jptxt.exe]");
        }
        LocalFree(argv);
        fs::create_directories(app->statePath.parent_path());
        auto lockPath = app->statePath; lockPath += L".lock";
        app->stateLock = CreateFileW(lockPath.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (app->stateLock == INVALID_HANDLE_VALUE) throw std::runtime_error("This session is already open or not writable. Use --session <another-file> for a separate window.");
        auto restored = load_state(app->statePath);
        if (restored.error) { app->notice = L"Session could not be restored; original preserved. " + wide(restored.error.message); app->saveAllowed = false; }
        if (restored.found && !restored.error && !explicitPaths) { app->core.restore(restored.state); app->width = restored.state.width; app->height = restored.state.height; }
        else app->core.start(left,right);
        INITCOMMONCONTROLSEX icc{sizeof(icc),ICC_LISTVIEW_CLASSES|ICC_TAB_CLASSES}; InitCommonControlsEx(&icc);
        WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.hInstance = instance; wc.lpfnWndProc = window_proc; wc.lpszClassName = L"aicmd-window";
        wc.hCursor = LoadCursorW(nullptr,IDC_ARROW); wc.hIcon = LoadIconW(nullptr,IDI_APPLICATION); wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
        RegisterClassExW(&wc);
        HWND hwnd = CreateWindowExW(0,wc.lpszClassName,L"aicmd — native file browser",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,app->width,app->height,nullptr,nullptr,instance,nullptr);
        if (!hwnd) throw std::runtime_error("Cannot create application window");
        ShowWindow(hwnd,show); render(); SetFocus(app->panes[app->core.state().activePanel].list);
        MSG msg{};
        while (GetMessageW(&msg,nullptr,0,0) > 0) { if (!key(msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
        if (!app->smokePath.empty()) { std::ofstream f(app->smokePath); f << (app->smokeFailed ? "FAIL" : "PASS") << " native GUI: compact full-width lists, tabs, parent, keyboard panel/tab/path/mark routes, persistence\n"; }
        int result = app->smokeFailed ? 1 : 0;
        CloseHandle(app->stateLock); DeleteObject(app->font); DeleteObject(app->activePath); DeleteObject(app->inactivePath); ImageList_Destroy(app->icons); app.reset(); return result;
    } catch (const std::exception& e) { MessageBoxW(nullptr,wide(e.what()).c_str(),L"aicmd",MB_ICONERROR); return 1; }
}
