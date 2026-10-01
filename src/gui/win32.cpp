#include "core/core.h"
#include "host/state.h"
#include "fs/ssh.h"
#include "media/session.h"
#include <nlohmann/json.hpp>
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
    bool active = false;
    size_t files = 0, dirs = 0;
};
struct App {
    Core core;
    HWND window{}, status{};
    HWND resumeFocus{};
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
    std::wstring selectionMask = L"*";
    bool selectionDirectories = true;
    bool locationMenuOpen = false;
    std::vector<std::string> sshHosts;
    Id transfer = 0;
    std::deque<std::pair<Id,bool>> transfers;
    std::wstring transferError;
    size_t transfersDone = 0, transfersCancelled = 0;
    int layoutPasses = 0;
    int drag = -1;
    RECT divider{};
    int width = 1200, height = 760;
    HANDLE mediaProcess{};
    fs::path mediaSession, mediaReturn;
    Location mediaDirectory;
    Id mediaTab = 0;
    int mediaPanel = 0;
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
std::wstring shown(const Location& p) { return wide(display_path(p)); }
std::wstring text(HWND h) { int n = GetWindowTextLengthW(h); std::wstring s(n+1,L'\0'); GetWindowTextW(h,s.data(),n+1); s.resize(n); return s; }
const Tab* current(int p) { return app->core.tab(p); }
void render();
void layout();
void save();
void run_command(Command c);
void capture(int p);
void restore_keyboard_target() {
    auto& pane = app->panes[app->core.state().activePanel];
    HWND target = app->resumeFocus;
    if ((target != pane.list && target != pane.tabs && target != pane.path) ||
        !IsWindowVisible(target) || !IsWindowEnabled(target)) target = pane.list;
    if (target && GetFocus() != target) SetFocus(target);
}
void redraw_row(HWND list,int row) {
    if (row < 0) return;
    RECT rect{}, client{};
    if (ListView_GetItemRect(list,row,&rect,LVIR_BOUNDS)) { GetClientRect(list,&client); rect.right = client.right; InvalidateRect(list,&rect,FALSE); }
}
void activate(int p);
std::wstring menu_path(const Location& path) {
    auto value = shown(path); std::wstring out;
    for (auto c : value) { out += c; if (c == L'&') out += c; }
    return out;
}
void location_menu(int p,bool drives) {
    capture(p); activate(p);
    auto menu = CreatePopupMenu();
    std::vector<Location> locations;
    if (drives) {
        wchar_t buffer[512]{};
        DWORD length = GetLogicalDriveStringsW(512,buffer);
        if (length && length < 512) for (auto s = buffer; *s; s += wcslen(s)+1) {
            locations.emplace_back(s);
            std::wstring label = L"&"; label += s[0]; label += L":\\";
            AppendMenuW(menu,MF_STRING,100+locations.size()-1,label.c_str());
        }
        Command discover; discover.action = Action::ConnectionsGet;
        app->sshHosts = app->core.execute(discover).connections; // Re-read config and Include files whenever the menu opens.
        auto ssh = CreatePopupMenu();
        for (const auto& host : app->sshHosts) {
            locations.push_back(Location::ssh(host));
            AppendMenuW(ssh,MF_STRING,100+locations.size()-1,wide(host).c_str());
        }
        if (app->sshHosts.empty()) AppendMenuW(ssh,MF_STRING|MF_GRAYED,0,L"No hosts in ~/.ssh/config");
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(ssh),L"&SSH");
    } else {
        locations = app->core.state().bookmarks;
        AppendMenuW(menu,MF_STRING,1,L"&Add current folder");
        bool present = std::find(locations.begin(),locations.end(),current(p)->path) != locations.end();
        AppendMenuW(menu,MF_STRING|(present ? 0 : MF_GRAYED),2,L"&Remove current folder");
        AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
        for (size_t i = 0; i < locations.size(); ++i) {
            auto label = (i < 9 ? L"&"+std::to_wstring(i+1)+L" " : L"")+menu_path(locations[i]);
            AppendMenuW(menu,MF_STRING,100+i,label.c_str());
        }
    }
    RECT rect{}; GetWindowRect(app->panes[p].pathLabel,&rect);
    app->locationMenuOpen = true;
    if (!app->smokePath.empty()) {
        ShowWindow(app->window,SW_SHOWNOACTIVATE);
        PostMessageW(app->window,WM_CHAR,drives && !locations.empty() ? locations[0].native()[0] : L'a',0);
    }
    UINT selected = TrackPopupMenuEx(menu,TPM_RETURNCMD|TPM_NONOTIFY|TPM_LEFTALIGN,rect.left,rect.bottom,app->window,nullptr);
    app->locationMenuOpen = false;
    DestroyMenu(menu); SetFocus(app->panes[p].list);
    Command c; c.panel = p;
    if (!drives && (selected == 1 || selected == 2)) {
        c.action = selected == 1 ? Action::BookmarkAdd : Action::BookmarkRemove;
        c.path = current(p)->path; run_command(c); save();
    } else if (selected >= 100 && selected-100 < locations.size()) {
        c.action = Action::Navigate; c.path = locations[selected-100]; run_command(c);
    }
}
void project_marks(int p) {
    const auto* t = current(p); auto& v = app->panes[p];
    if (!t || !v.snapshot || t->snapshot != v.snapshot) return;
    app->syncing = true;
    ListView_SetItemState(v.list,0,0,LVIS_SELECTED);
    for (size_t i = 0; i < v.snapshot->entries.size(); ++i) {
        UINT wanted = t->selection.count(v.snapshot->entries[i].name) ? LVIS_SELECTED : 0;
        if (ListView_GetItemState(v.list,static_cast<int>(i)+1,LVIS_SELECTED) != wanted) ListView_SetItemState(v.list,static_cast<int>(i)+1,wanted,LVIS_SELECTED);
    }
    app->syncing = false; v.selectionDirty = false; InvalidateRect(v.list,nullptr,FALSE);
}
INT_PTR CALLBACK selection_dialog(HWND h,UINT message,WPARAM w,LPARAM l) {
    if (message == WM_INITDIALOG) {
        SetWindowTextW(h,l < 0 ? L"Unselect files" : L"Select files");
        SetDlgItemTextW(h,2002,app->selectionMask.c_str()); SendDlgItemMessageW(h,2002,EM_SETLIMITTEXT,256,0);
        CheckDlgButton(h,2003,app->selectionDirectories ? BST_CHECKED : BST_UNCHECKED);
        if (!app->smokePath.empty()) { SetDlgItemTextW(h,2002,L"*.md;*.txt"); PostMessageW(h,WM_COMMAND,IDOK,0); }
        SendDlgItemMessageW(h,2002,EM_SETSEL,0,-1); return TRUE;
    }
    if (message == WM_COMMAND && (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL)) {
        if (LOWORD(w) == IDOK) { wchar_t pattern[257]{}; GetDlgItemTextW(h,2002,pattern,257); if (!pattern[0]) return TRUE; app->selectionMask = pattern; app->selectionDirectories = IsDlgButtonChecked(h,2003) == BST_CHECKED; }
        EndDialog(h,LOWORD(w)); return TRUE;
    }
    return FALSE;
}
void mark_pattern(int p,int mode,bool dialog) {
    capture(p);
    if (dialog) {
        auto result = DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(2001),app->window,selection_dialog,mode);
        SetFocus(app->panes[p].list);
        if (result != IDOK) return;
    }
    auto t = current(p); if (!t || !t->snapshot) return;
    Command c; c.action = Action::SelectionPattern; c.panel = p; c.tab = t->id; c.snapshot = t->snapshot->id;
    c.value = mode; c.flag = dialog ? app->selectionDirectories : true; c.pattern = dialog ? fs::path(app->selectionMask).u8string() : "*";
    run_command(c); project_marks(p);
}
void edit_path(int p) {
    auto& v = app->panes[p];
    v.editingPath = true;
    ShowWindow(v.pathLabel,SW_HIDE); ShowWindow(v.path,SW_SHOW);
    SetWindowTextW(v.path,current(p)->path.native().c_str());
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
bool editor_protocol2(const fs::path& executable) {
    DWORD unused = 0, size = GetFileVersionInfoSizeW(executable.c_str(),&unused);
    if (!size || size > 1024*1024) return false;
    std::vector<unsigned char> data(size);
    if (!GetFileVersionInfoW(executable.c_str(),0,size,data.data())) return false;
    wchar_t* protocol = nullptr; UINT len = 0;
    return VerQueryValueW(data.data(),L"\\StringFileInfo\\040904b0\\JptxtReadOnlyProtocol",reinterpret_cast<void**>(&protocol),&len) && len >= 2 && protocol && !wcscmp(protocol,L"2");
}
void launch_editor(const fs::path& file,bool viewOnly) {
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
    bool modern = editor_protocol2(executable);
    if (viewOnly && !modern) { app->notice = L"F3 requires jptxt 0.2.0+ with read-only support. Configure the updated jptxt executable."; return; }
    if (!modern) for (wchar_t c : file.native()) if (c < 32) { app->notice = L"This older jptxt cannot receive control characters in paths."; return; }
    std::wstring args = quote_arg(executable.native())+(modern ? (viewOnly ? L" --readonly -- " : L" --edit -- ") : L" ")+quote_arg(file.native());
    STARTUPINFOW si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
    if (!CreateProcessW(executable.c_str(),args.data(),nullptr,nullptr,FALSE,0,nullptr,file.parent_path().c_str(),&si,&pi)) {
        app->notice = L"Cannot launch jptxt (Windows error " + std::to_wstring(GetLastError()) + L")."; return;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    app->notice = viewOnly ? L"Opened read-only in jptxt. F4 there enables editing without restarting." : L"Opened in jptxt. Ctrl+R refreshes the listing.";
}
bool launch_media(int p) {
    auto& pane = app->panes[p]; auto tab = current(p);
    int row = ListView_GetNextItem(pane.list,-1,LVNI_FOCUSED);
    if (!tab || !tab->snapshot || row <= 0 || static_cast<size_t>(row) > tab->snapshot->entries.size()) return false;
    const auto& focused = tab->snapshot->entries[row-1];
    if (focused.directory || av::kind(focused.name) == av::Kind::Unknown) return false;
    if (app->mediaProcess) {app->notice = L"A media viewer is already open for this commander. Close it before opening another folder."; return true;}
    capture(p);
    try {
        wchar_t module[32768]{}; GetModuleFileNameW(nullptr,module,32768);
        auto executable = fs::path(module).parent_path()/L"aiview.exe";
        if (!fs::is_regular_file(executable)) throw std::runtime_error("aiview.exe not found beside aicmd.exe; build the optional media viewer");
        nlohmann::json paths = nlohmann::json::array(); size_t index = 0;
        for (const auto& e : tab->snapshot->entries) if (!e.directory && av::kind(e.name) != av::Kind::Unknown) {
            if (e.name == focused.name) index = paths.size(); paths.push_back((tab->path/e.name).u8string());
        }
        auto payload = nlohmann::json{{"version",1},{"items",paths},{"index",index}}.dump();
        if (payload.size() > 32*1024*1024) throw std::runtime_error("Media folder snapshot exceeds 32 MiB");
        auto root = fs::temp_directory_path()/L"aicmd-media"; fs::create_directories(root);
        auto name = std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
        app->mediaSession = root/(name+L".json"); app->mediaReturn = root/(name+L".return.json");
        HANDLE file = CreateFileW(app->mediaSession.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY,nullptr);
        if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create media folder snapshot");
        DWORD written = 0; bool saved = WriteFile(file,payload.data(),static_cast<DWORD>(payload.size()),&written,nullptr) && written == payload.size(); CloseHandle(file);
        if (!saved) throw std::runtime_error("Cannot write media folder snapshot");
        std::wstring args = quote_arg(executable.native())+L" --session "+quote_arg(app->mediaSession.native())+L" --return "+quote_arg(app->mediaReturn.native());
        STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
        if (!CreateProcessW(executable.c_str(),args.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&si,&pi)) throw std::runtime_error("Cannot launch aiview");
        CloseHandle(pi.hThread); app->mediaProcess = pi.hProcess; app->mediaPanel = p; app->mediaTab = tab->id; app->mediaDirectory = tab->path;
        app->notice = L"Media viewer opened. PgUp/PgDn browse; Ctrl+C copies the image/frame. Insert or Shift+Space marks media.";
    } catch (const std::exception& e) {
        app->notice = wide(e.what()); std::error_code ec; fs::remove(app->mediaSession,ec); fs::remove(app->mediaReturn,ec);
    }
    return true;
}
void poll_media() {
    if (!app->mediaProcess || WaitForSingleObject(app->mediaProcess,0) != WAIT_OBJECT_0) return;
    const auto* tab = current(app->mediaPanel);
    if (tab && tab->id == app->mediaTab && tab->loading) return;
    try {
        if (tab && tab->id == app->mediaTab && tab->path == app->mediaDirectory && tab->snapshot && fs::exists(app->mediaReturn) && fs::file_size(app->mediaReturn) < 65536) {
            std::ifstream input(app->mediaReturn,std::ios::binary); nlohmann::json j; input >> j;
            Location file(fs::u8path(j.at("path").get<std::string>()));
            if (file.parent_path() == tab->path) {
                Command c; c.action = Action::SelectionSet; c.panel = app->mediaPanel; c.tab = tab->id; c.snapshot = tab->snapshot->id; c.selection = tab->selection; c.focus = file.filename(); app->core.execute(c);
                app->panes[app->mediaPanel].snapshot.reset(); render();
            }
        }
    } catch (...) {}
    CloseHandle(app->mediaProcess); app->mediaProcess = nullptr;
    std::error_code ec; fs::remove(app->mediaSession,ec); fs::remove(app->mediaReturn,ec);
    // WM_ACTIVATE/WM_SETFOCUS recover keyboard routing without stealing foreground.
    app->notice = L"Media viewer closed.";
}
void run_command(Command c) {
    auto r = app->core.execute(c);
    if (r.error) app->notice = wide(r.error.message);
    else { app->notice.clear(); if (!r.launchPath.empty()) launch_editor(r.launchPath.local,r.viewOnly); }
    if (!r.error && r.job && (c.action == Action::FileCopy || c.action == Action::FileView)) {
        if (app->transfers.empty()) {app->transferError.clear(); app->transfersDone = app->transfersCancelled = 0;}
        app->transfers.emplace_back(r.job,c.action == Action::FileCopy);
        app->transfer = app->transfers.front().first;
        app->notice = L"Transfer queued. Escape cancels current; Shift+Escape cancels all.";
    }
    if (r.save) save();
    if (app->core.state().quit) PostMessageW(app->window,WM_CLOSE,0,0);
    app->dirty = true;
}
void action(Action a, int p = -1) {
    if (p < 0) p = app->core.state().activePanel;
    capture(p);
    if (a == Action::FileCopy) {
        auto prompt = L"Copy marked entries (or the focused entry) to:\n"+shown(current(1-p)->path)+L"\n\nExisting destinations will not be overwritten.";
        if (MessageBoxW(app->window,prompt.c_str(),L"Copy - F5",MB_OKCANCEL|MB_ICONQUESTION) != IDOK) return;
    }
    Command c; c.action = a; c.panel = p;
    if (const auto* t = current(p)) {
        c.tab = t->id; c.options = t->options;
        if (a == Action::FilterSet) c.options.hidden = !c.options.hidden;
        if (a == Action::FileEdit || a == Action::FileView || a == Action::FileCopy) { c.path = t->focus; c.snapshot = t->snapshot ? t->snapshot->id : 0; }
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
    c.action = e.directory ? Action::Navigate : v.snapshot->path.is_remote() ? Action::FileView : Action::FileEdit;
    c.path = e.directory ? v.snapshot->path / e.name : e.name;
    run_command(c);
}
void go(int p) { capture(p); Command c; c.action = Action::Navigate; c.panel = p; c.path = text(app->panes[p].path); run_command(c); end_path(p); }
// List-view selection is our persistent mark set, not the navigation cursor.
// Own the complete left-button sequence so native selection/drag tracking cannot
// clear marks or limit clicks to the filename label after keyboard navigation.
LRESULT CALLBACK file_list_proc(HWND list,UINT message,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR) {
    if (message == WM_NCDESTROY) { RemoveWindowSubclass(list,file_list_proc,id); return DefSubclassProc(list,message,w,l); }
    if (message == WM_LBUTTONUP) return 0;
    if (message != WM_LBUTTONDOWN && message != WM_LBUTTONDBLCLK) return DefSubclassProc(list,message,w,l);
    int p = static_cast<int>(id); auto& pane = app->panes[p];
    int previousPanel = app->core.state().activePanel;
    if (app->panes[previousPanel].selectionDirty) capture(previousPanel);
    if (pane.editingPath) end_path(p);
    activate(p); SetFocus(list);
    POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)}; RECT client{}; GetClientRect(list,&client);
    int row = -1;
    if (PtInRect(&client,point)) {
        const int count = ListView_GetItemCount(list), first = ListView_GetTopIndex(list);
        const int end = std::min(count,first+ListView_GetCountPerPage(list)+2);
        for (int i = first;i < end;++i) {
            RECT bounds{};
            if (ListView_GetItemRect(list,i,&bounds,LVIR_BOUNDS) && point.y >= bounds.top && point.y < bounds.bottom) {row = i; break;}
        }
    }
    if (row < 0) return 0;
    int old = ListView_GetNextItem(list,-1,LVNI_FOCUSED);
    if (w & MK_SHIFT) {
        if (old < 0) old = row;
        for (int i = std::max(1,std::min(old,row));i <= std::max(old,row);++i) {
            ListView_SetItemState(list,i,LVIS_SELECTED,LVIS_SELECTED); redraw_row(list,i);
        }
    } else if ((w & MK_CONTROL) && row > 0 && message == WM_LBUTTONDOWN) {
        bool marked = (ListView_GetItemState(list,row,LVIS_SELECTED)&LVIS_SELECTED) != 0;
        ListView_SetItemState(list,row,marked ? 0 : LVIS_SELECTED,LVIS_SELECTED);
    }
    ListView_SetItemState(list,row,LVIS_FOCUSED,LVIS_FOCUSED);
    ListView_EnsureVisible(list,row,FALSE);
    pane.selectionDirty = true; redraw_row(list,old); redraw_row(list,row);
    if (message == WM_LBUTTONDBLCLK && !(w & (MK_CONTROL|MK_SHIFT))) open_item(p);
    return 0;
}
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
        SetWindowSubclass(v.list,file_list_proc,static_cast<UINT_PTR>(p),0);
        ListView_SetExtendedListViewStyle(v.list,LVS_EX_DOUBLEBUFFER|LVS_EX_LABELTIP);
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
    add(file,Action::FileView,L"Read-only &viewer\tF3");
    add(file,Action::FileCopy,L"&Copy to other panel\tF5");
    AppendMenuW(file,MF_SEPARATOR,0,nullptr); add(file,Action::SessionSave,L"&Save session"); add(file,Action::AppQuit,L"E&xit");
    HMENU view = CreatePopupMenu(); add(view,Action::Refresh,L"&Refresh / Retry\tCtrl+R"); add(view,Action::FilterSet,L"Show &hidden files\tCtrl+H");
    HMENU tabs = CreatePopupMenu(); add(tabs,Action::TabCreate,L"&New tab\tCtrl+T"); add(tabs,Action::TabClose,L"&Close tab\tCtrl+W"); add(tabs,Action::TabCycle,L"Ne&xt tab\tCtrl+Tab");
    AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(file),L"&File"); AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(view),L"&View"); AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(tabs),L"&Tabs");
    SetMenu(app->window,bar);
}
void layout() {
    ++app->layoutPasses;
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
            ids.push_back(tab.id); auto label = shown(tab.path.filename().empty() ? tab.path : Location(tab.path.filename()));
            if (tab.loading) label += L" ..."; if (tab.error) label += L" !"; labels.push_back(label);
        }
        if (ids != v.tabIds || labels != v.tabLabels) {
            SendMessageW(v.tabs,WM_SETREDRAW,FALSE,0); TabCtrl_DeleteAllItems(v.tabs);
            for (size_t i = 0; i < ids.size(); ++i) { TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = labels[i].data(); TabCtrl_InsertItem(v.tabs,static_cast<int>(i),&item); }
            v.tabIds = ids; v.tabLabels = labels;
            SendMessageW(v.tabs,WM_SETREDRAW,TRUE,0); InvalidateRect(v.tabs,nullptr,TRUE);
        }
        auto selected = std::find(ids.begin(),ids.end(),t->id);
        int selectedIndex = static_cast<int>(selected-ids.begin());
        if (TabCtrl_GetCurSel(v.tabs) != selectedIndex) TabCtrl_SetCurSel(v.tabs,selectedIndex);
        if (switched || GetFocus() != v.path) {
            auto path = t->loading ? t->requested : t->path;
            auto native = path.native(); if (text(v.path) != native) SetWindowTextW(v.path,native.c_str());
        }
        auto bar = L" " + shown(t->loading ? t->requested : t->path);
        if (t->loading) bar += L"  [loading]";
        bool active = p == app->core.state().activePanel;
        if (text(v.pathLabel) != bar) SetWindowTextW(v.pathLabel,bar.c_str());
        if (v.active != active) { v.active = active; InvalidateRect(v.pathLabel,nullptr,FALSE); InvalidateRect(v.list,nullptr,FALSE); }
        if (switched || v.snapshot != t->snapshot) {
            v.tab = t->id; v.snapshot = t->snapshot;
            v.files = v.dirs = 0;
            if (t->snapshot) for (const auto& e : t->snapshot->entries) { if (e.directory) ++v.dirs; else ++v.files; }
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
        std::wstring summary = std::to_wstring(t->selection.size())+L" selected / "+std::to_wstring(v.files)+L" file(s), "+std::to_wstring(v.dirs)+L" dir(s)";
        if (t->error) summary = wide(t->error.message)+L"  |  Ctrl+R Retry";
        if (text(v.summary) != summary) SetWindowTextW(v.summary,summary.c_str());
        HWND header = ListView_GetHeader(v.list);
        for (int i = 0; i < 5; ++i) {
            HDITEMW item{}; item.mask = HDI_FORMAT; Header_GetItem(header,i,&item);
            int previous = item.fmt;
            item.fmt &= ~(HDF_SORTUP|HDF_SORTDOWN);
            if (i == static_cast<int>(t->options.sort)) item.fmt |= t->options.descending ? HDF_SORTDOWN : HDF_SORTUP;
            if (previous != item.fmt) Header_SetItem(header,i,&item);
        }
    }
    const auto* t = current(app->core.state().activePanel);
    std::wstring status = app->notice;
    if (status.empty() && t && t->error) status = wide(t->error.message)+L"   |   Ctrl+R Retry";
    if (status.empty()) status = L"F3 View   F4 Edit   F5 Copy   Tab Panel   Insert Mark   Num+ Select   Num- Unselect   Ctrl+D Bookmarks   Alt+F1/F2 Drives/SSH";
    if (text(app->status) != status) SetWindowTextW(app->status,status.c_str());
    CheckMenuItem(GetMenu(app->window),MENU_BASE+static_cast<UINT>(Action::FilterSet),MF_BYCOMMAND|(t && t->options.hidden ? MF_CHECKED : MF_UNCHECKED));
    app->syncing = false; app->seen = app->core.state().revision; app->renderedNotice = app->notice;
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
                    draw->nmcd.uItemState &= ~(CDIS_SELECTED|CDIS_FOCUS);
                    draw->clrTextBk = focused ? RGB(0,28,44) : GetSysColor(COLOR_WINDOW);
                    draw->clrText = marked ? (focused ? RGB(255,100,100) : RGB(192,0,0)) : focused ? RGB(255,255,255) : GetSysColor(COLOR_WINDOWTEXT);
                    RECT rowRect{}, client{}; ListView_GetItemRect(v.list,row,&rowRect,LVIR_BOUNDS); GetClientRect(v.list,&client);
                    rowRect.right = client.right;
                    auto brush = CreateSolidBrush(draw->clrTextBk); FillRect(draw->nmcd.hdc,&rowRect,brush); DeleteObject(brush);
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
                if (hdr->code == LVN_ITEMCHANGED) redraw_row(v.list,reinterpret_cast<NMLISTVIEW*>(hdr)->iItem);
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
    // An active top-level window is not necessarily focused on an input control.
    // Recover navigation routing, but never redirect keys from the path editor.
    if ((focus == app->window || !focus) && GetActiveWindow() == app->window) {
        restore_keyboard_target(); focus = GetFocus();
    }
    for (int i = 0; i < 2; ++i) { auto& v = app->panes[i]; if (focus == v.path || focus == v.tabs || focus == v.list) p = i; }
    auto& v = app->panes[p];
    if (alt && (msg.wParam == VK_F1 || msg.wParam == VK_F2)) { location_menu(msg.wParam == VK_F1 ? 0 : 1,true); return true; }
    if (ctrl && msg.wParam == 'D') { location_menu(p,false); return true; }
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
        if (msg.wParam == VK_SPACE && !shift && !ctrl && !alt && launch_media(p)) return true;
        int row = ListView_GetNextItem(v.list,-1,LVNI_FOCUSED);
        if (row > 0) { bool selected = (ListView_GetItemState(v.list,row,LVIS_SELECTED)&LVIS_SELECTED) != 0; ListView_SetItemState(v.list,row,selected ? 0 : LVIS_SELECTED,LVIS_SELECTED); }
        if (msg.wParam == VK_INSERT && row+1 < ListView_GetItemCount(v.list)) {
            ListView_SetItemState(v.list,row+1,LVIS_FOCUSED,LVIS_FOCUSED); ListView_EnsureVisible(v.list,row+1,FALSE);
        }
        v.selectionDirty = true; redraw_row(v.list,row); redraw_row(v.list,row+1); return true;
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
        v.selectionDirty = true; redraw_row(v.list,row); redraw_row(v.list,next); return true;
    }
    if (ctrl && msg.wParam == 'A' && focus == v.list) { mark_pattern(p,1,false); return true; }
    if (focus == v.list && msg.wParam == VK_ADD) { mark_pattern(p,1,!shift); return true; }
    if (focus == v.list && msg.wParam == VK_SUBTRACT) { mark_pattern(p,-1,!shift); return true; }
    if (focus == v.list && msg.wParam == VK_MULTIPLY) { mark_pattern(p,0,false); return true; }
    if (msg.wParam == VK_F4 && !alt) { action(Action::FileEdit,p); return true; }
    if (msg.wParam == VK_F3) { action(Action::FileView,p); return true; }
    if (msg.wParam == VK_F5) { action(Action::FileCopy,p); return true; }
    if (msg.wParam == VK_ESCAPE) {
        if (focus == v.path) end_path(p);
        else if (app->transfer) {
            Command c; c.action = Action::JobCancel; c.panel = p;
            if (shift) {for (const auto& transfer : app->transfers) {c.job = transfer.first; run_command(c);}}
            else {c.job = app->transfer; run_command(c);}
        } else action(Action::JobCancel,p);
        return true;
    }
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
    if (app->smokePath.empty() || app->locationMenuOpen) return;
    if (++app->smokeTicks > 600) { app->smokeFailed = true; PostMessageW(app->window,WM_CLOSE,0,0); return; }
    if (app->core.busy()) return;
    auto check = [](bool okay) { static int index = 0; ++index; if (!okay) { app->smokeFailed = true; std::ofstream log(app->smokePath.string()+".failures",std::ios::app); log << "stage " << app->smokeStage << " check " << index << '\n'; } };
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
        ShowWindow(app->window,SW_SHOWNOACTIVATE);
        HWND list = app->panes[1].list; SetFocus(list);
        // Reproduce Windows returning focus to the top-level window after an
        // external app exits. Navigation needs the child control, not just
        // foreground ownership or a painted row cursor.
        SendMessageW(app->window,WM_ACTIVATE,WA_INACTIVE,0);
        SetFocus(app->window); check(GetFocus() == list);
        press('L',1);
        SetWindowTextW(app->panes[1].path,L"unfinished path");
        SendMessageW(app->window,WM_ACTIVATE,WA_INACTIVE,0);
        SetFocus(app->window);
        check(GetFocus() == app->panes[1].path && text(app->panes[1].path) == L"unfinished path");
        press(VK_ESCAPE);
        SetFocus(app->panes[1].tabs);
        SendMessageW(app->window,WM_ACTIVATE,WA_INACTIVE,0);
        SetFocus(app->window); check(GetFocus() == app->panes[1].tabs);
        SetFocus(list);
        int layouts = app->layoutPasses;
        for (int i = 0; i < 50; ++i) { press(VK_DOWN); capture(1); render(); press(VK_UP); capture(1); render(); }
        check(app->layoutPasses == layouts);
        auto click = [&](HWND target,int row,int x,WPARAM modifiers = 0,UINT message = WM_LBUTTONDOWN) {
            ListView_EnsureVisible(target,row,FALSE); RECT bounds{};
            check(ListView_GetItemRect(target,row,&bounds,LVIR_BOUNDS) != FALSE);
            auto point = MAKELPARAM(x,(bounds.top+bounds.bottom)/2);
            SendMessageW(target,message,modifiers|MK_LBUTTON,point);
            SendMessageW(target,WM_LBUTTONUP,modifiers,point);
        };
        ListView_SetItemState(list,-1,0,LVIS_SELECTED|LVIS_FOCUSED);
        ListView_SetItemState(list,1,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
        RECT mouseClient{}; GetClientRect(list,&mouseClient);
        // Alternate arrows and clicks across the name, metadata and blank row tail.
        for (int x : {px(25),px(300),static_cast<int>(mouseClient.right)-px(4)}) {
            press(VK_HOME); press(VK_DOWN); press(VK_DOWN);
            click(list,4,x);
            check(ListView_GetNextItem(list,-1,LVNI_FOCUSED) == 4);
            check(ListView_GetSelectedCount(list) == 1 && (ListView_GetItemState(list,1,LVIS_SELECTED)&LVIS_SELECTED));
            check(GetCapture() != list);
        }
        click(list,4,px(25),MK_CONTROL); check(ListView_GetSelectedCount(list) == 2);
        click(list,4,px(25),MK_CONTROL); check(ListView_GetSelectedCount(list) == 1);
        click(list,6,mouseClient.right-px(4),MK_SHIFT);
        check(ListView_GetSelectedCount(list) == 4); // Original mark plus rows 4..6.
        click(list,0,px(25),MK_CONTROL); check(!(ListView_GetItemState(list,0,LVIS_SELECTED)&LVIS_SELECTED));
        click(app->panes[0].list,2,px(300));
        check(app->core.state().activePanel == 0 && GetFocus() == app->panes[0].list);
        click(list,3,px(300)); check(app->core.state().activePanel == 1 && GetFocus() == list);
        capture(1); check(current(1)->selection.size() == 4);
        RECT lastRow{};
        if (ListView_GetItemRect(list,ListView_GetItemCount(list)-1,&lastRow,LVIR_BOUNDS) && lastRow.bottom < mouseClient.bottom-px(2)) {
            auto point = MAKELPARAM(px(50),mouseClient.bottom-px(1));
            SendMessageW(list,WM_LBUTTONDOWN,MK_LBUTTON,point); SendMessageW(list,WM_LBUTTONUP,0,point);
            check(ListView_GetNextItem(list,-1,LVNI_FOCUSED) == 3 && ListView_GetSelectedCount(list) == 4);
        }
        ListView_SetItemState(list,-1,0,LVIS_SELECTED|LVIS_FOCUSED);
        ListView_SetItemState(list,1,LVIS_FOCUSED,LVIS_FOCUSED);
        press(VK_INSERT); press(VK_DOWN);
        check(ListView_GetSelectedCount(list) == 1);
        check(ListView_GetNextItem(list,-1,LVNI_FOCUSED) == 3);
        capture(1); check(current(1)->selection.size() == 1);
        press('A',1); check(current(1)->selection.size() == current(1)->snapshot->entries.size());
        press(VK_SUBTRACT,2); check(current(1)->selection.empty());
        press(VK_ADD); auto marked = current(1)->selection.size();
        check(marked > 0 && marked < current(1)->snapshot->entries.size());
        press(VK_MULTIPLY); check(current(1)->selection.size() == current(1)->snapshot->entries.size()-marked);
        press(VK_SUBTRACT,2); check(current(1)->selection.empty());
        press('L',1); press(VK_ESCAPE); check(!app->panes[1].editingPath);
        press('D',1); check(app->core.state().bookmarks.size() == 1);
        press(VK_F1,4); check(app->core.state().activePanel == 0); check(current(0)->requested == current(0)->requested.root_path());
        press(VK_F2,4); check(app->core.state().activePanel == 1); check(current(1)->requested == current(1)->requested.root_path());
        save();
        auto persisted = load_state(app->statePath);
        check(persisted.found && !persisted.error && persisted.state.panels[0].tabs.size() == 1);
        check(persisted.state.bookmarks.size() == 1);
    } else if (app->smokeStage == 7) {
        Command c; c.action = Action::Navigate; c.panel = 1; c.path = fs::current_path(); run_command(c);
    } else if (app->smokeStage == 8) {
        check(current(1)->path == fs::current_path());
        HWND list = app->panes[1].list; ListView_EnsureVisible(list,0,FALSE);
        RECT row{},client{}; ListView_GetItemRect(list,0,&row,LVIR_BOUNDS); GetClientRect(list,&client);
        auto point = MAKELPARAM(client.right-px(4),(row.top+row.bottom)/2);
        SendMessageW(list,WM_LBUTTONDOWN,MK_LBUTTON,point); SendMessageW(list,WM_LBUTTONUP,0,point);
        SendMessageW(list,WM_LBUTTONDBLCLK,MK_LBUTTON,point); SendMessageW(list,WM_LBUTTONUP,0,point);
        check(current(1)->requested == fs::current_path().parent_path());
    } else if (app->smokeStage == 9) {
        check(current(1)->path == fs::current_path().parent_path());
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
        poll_media();
        for (int p = 0; p < 2; ++p) if (app->panes[p].selectionDirty) capture(p);
        if (app->core.poll()) app->dirty = true;
        for (const auto& path : app->core.take_viewers()) {
            launch_editor(path,true);
            app->notice = L"Remote file staged read-only in jptxt. Editing this local copy does not upload changes.";
        }
        if (app->transfer) {
            for (auto it = app->transfers.begin(); it != app->transfers.end();) {
                const auto* job = app->core.job(it->first);
                if (job && job_pending(job->state)) {++it; continue;}
                if (!job) app->transferError = L"Transfer result expired.";
                else {
                    if (job->error) app->transferError = L"Job #"+std::to_wstring(job->id)+L": "+wide(job->error.message);
                    else if (job->state == JobState::Cancelled) ++app->transfersCancelled;
                    else ++app->transfersDone;
                    auto destination = job->destination;
                    if (it->second) for (int p = 0;p < 2;++p) {
                        const auto* tab = current(p);
                        if (tab && !tab->loading && tab->path == destination) {Command c; c.action = Action::Refresh; c.panel = p; app->core.execute(c);}
                    }
                }
                it = app->transfers.erase(it);
            }
            app->transfer = app->transfers.empty() ? 0 : app->transfers.front().first;
            if (app->transfer) {
                const auto* job = app->core.job(app->transfer);
                auto phase = job->state == JobState::Queued ? L"Queued" : job->state == JobState::Cancelling ? L"Cancelling" : app->transfers.front().second ? L"Copying" : L"Downloading viewer file";
                app->notice = std::wstring(phase)+L" #"+std::to_wstring(job->id)+L": "+std::to_wstring(job->bytes->load())+L" bytes";
                if (job->progress) {
                    app->notice += L", "+std::to_wstring(job->progress->completedEntries.load())+L"/"+std::to_wstring(job->progress->totalEntries)+L" selected entries";
                    std::lock_guard<std::mutex> lock(job->progress->mutex);
                    app->notice += L" | "+shown(job->progress->current.filename());
                }
                app->notice += L" | "+std::to_wstring(app->transfers.size()-1)+L" pending | Esc cancel, Shift+Esc cancel all";
                if (!app->transferError.empty()) app->notice += L" | Earlier error: "+app->transferError;
            } else {
                app->notice = L"Transfers finished: "+std::to_wstring(app->transfersDone)+L" completed, "+std::to_wstring(app->transfersCancelled)+L" cancelled. "+app->transferError;
                if (app->transfersCancelled || !app->transferError.empty()) app->notice += L" Completed files remain; remote staging files may need cleanup after disconnect.";
            }
        }
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
    case WM_SETFOCUS:
        restore_keyboard_target(); return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) {
            HWND focus = GetFocus();
            if (focus && IsChild(hwnd,focus)) app->resumeFocus = focus;
        }
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
        app->sshHosts = ssh_hosts();
        app->statePath = default_state_path();
        if (auto e = _wgetenv(L"AICMD_EDITOR")) app->editor = e;
        int argc = 0; auto argv = CommandLineToArgvW(GetCommandLineW(),&argc);
        Location left = fs::current_path(), right = left; bool explicitPaths = false;
        for (int i = 1; i < argc; ++i) {
            std::wstring arg = argv[i];
            if ((arg == L"--session" || arg == L"--editor" || arg == L"--smoke") && i+1 < argc) {
                fs::path value = absolute_path(argv[++i],fs::current_path());
                if (arg == L"--session") app->statePath = value;
                if (arg == L"--editor") app->editor = value;
                if (arg == L"--smoke") { app->smokePath = value; app->statePath = value; app->statePath += L".session.json"; explicitPaths = true; }
            } else if (!arg.empty() && arg[0] != L'-') { if (!explicitPaths) left = right = absolute_path(Location(fs::path(arg)),Location(fs::current_path())); else right = absolute_path(Location(fs::path(arg)),Location(fs::current_path())); explicitPaths = true; }
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
        else {
            app->core.start(left,right);
            if (restored.found && !restored.error && !restored.state.bookmarks.empty()) {
                auto state = app->core.save(); state.bookmarks = restored.state.bookmarks; app->core.restore(state);
            }
        }
        INITCOMMONCONTROLSEX icc{sizeof(icc),ICC_LISTVIEW_CLASSES|ICC_TAB_CLASSES}; InitCommonControlsEx(&icc);
        WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.hInstance = instance; wc.lpfnWndProc = window_proc; wc.lpszClassName = L"aicmd-window";
        wc.hCursor = LoadCursorW(nullptr,IDC_ARROW); wc.hIcon = LoadIconW(nullptr,IDI_APPLICATION); wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
        RegisterClassExW(&wc);
        HWND hwnd = CreateWindowExW(0,wc.lpszClassName,L"aicmd — native file browser",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,app->width,app->height,nullptr,nullptr,instance,nullptr);
        if (!hwnd) throw std::runtime_error("Cannot create application window");
        ShowWindow(hwnd,show); render(); SetFocus(app->panes[app->core.state().activePanel].list);
        MSG msg{};
        while (GetMessageW(&msg,nullptr,0,0) > 0) { if (!key(msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
        if (!app->smokePath.empty()) { std::ofstream f(app->smokePath); f << (app->smokeFailed ? "FAIL" : "PASS") << " native GUI: compact full-width lists, tabs, parent, keyboard/mouse cursor and mark routes, full-row clicks, double-click navigation, persistence\n"; }
        int result = app->smokeFailed ? 1 : 0;
        CloseHandle(app->stateLock); DeleteObject(app->font); DeleteObject(app->activePath); DeleteObject(app->inactivePath); ImageList_Destroy(app->icons); app.reset(); return result;
    } catch (const std::exception& e) { MessageBoxW(nullptr,wide(e.what()).c_str(),L"aicmd",MB_ICONERROR); return 1; }
}
