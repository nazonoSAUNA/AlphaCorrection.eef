#include <windows.h>
#include <algorithm>
#include <exedit.hpp>

inline static char name[] = "アルファ値補正";
constexpr int track_n = 3;
static char* track_name[track_n] = { const_cast<char*>("値"), const_cast<char*>("閾値-上"), const_cast<char*>("閾値-下") };
static int track_default[track_n] = { 0x800, 0x1000, 0 };
static int track_s[track_n] = { 0, 0, 0 };
static int track_e[track_n] = { 0x2000, 0x1000, 0x1000 };
static int track_scale[track_n] = { 1, 1, 1};
static int track_drag_min[track_n] = { 0, 0, 0 };
static int track_drag_max[track_n] = { 0x1000, 0x1000, 0x1000 };

constexpr int check_n = 2;
constexpr int mode_n = 5;
static char* check_name[check_n] = { const_cast<char*>("加算\0減算\0乗算\0スクリーン\0上書き\0"), const_cast<char*>("透明色の設定") };

constexpr int check_type_button = -1;
constexpr int check_type_dropdown = -2;
static int check_default[check_n] = { check_type_dropdown, check_type_button };

constexpr int exdata_size = 8;
static char exdata_def[exdata_size] = { 0, 0, 0, 0, 0, 0, 0, 1 };
static ExEdit::ExdataUse exdata_use[] = {
    {
        .type = ExEdit::ExdataUse::Type::Number,
        .size = 4,
        .name = "mode",
    },
    {
        .type = ExEdit::ExdataUse::Type::Binary,
        .size = 3,
        .name = "color",
    },
    {
        .type = ExEdit::ExdataUse::Type::Number,
        .size = 1,
        .name = "no_color",
    }
};
struct Exdata {
    int mode;
    int color;
};

struct AlphaControl {
    void(__cdecl* func)(ExEdit::PixelYCA* dst, int v);
	int v;
	short y, cb, cr;
	short threshold_min;
	short threshold_max;
};


int get_exedit92_dll_hinst(ExEdit::Filter* efp) {
    constexpr int exedit92_exfunc_address = 0xa41e0;
    return (int)efp->exfunc - exedit92_exfunc_address;
}


void add(ExEdit::PixelYCA* dst, int v) {
    dst->a = min(dst->a + v, 0x1000);
}
void sub(ExEdit::PixelYCA* dst, int v) {
    dst->a = max(0, dst->a - v);
}
void mul(ExEdit::PixelYCA* dst, int v) {
    dst->a = std::clamp(dst->a * v >> 12, 0, 0x1000);
}
void screen(ExEdit::PixelYCA* dst, int v) {
    dst->a = std::clamp(dst->a + v - (dst->a * v >> 12), 0, 0x1000);
}
void wrap(ExEdit::PixelYCA* dst, int v) {
    dst->a = v;
}
void mt(int thread_id, int thread_num, AlphaControl* ac, ExEdit::FilterProcInfo* efpip) {
    int y = efpip->obj_h * thread_id / thread_num;
    auto dst = (ExEdit::PixelYCA*)efpip->obj_edit + y * efpip->obj_line;
    int line = (efpip->obj_line - efpip->obj_w) * sizeof(ExEdit::PixelYCA);
    for (y = efpip->obj_h * (thread_id + 1) / thread_num - y; 0 < y; y--) {
        for (int x = efpip->obj_w; 0 < x; x--) {
            if (ac->threshold_min <= dst->a && dst->a <= ac->threshold_max) {
                ac->func(dst, ac->v);
            }
            dst++;
        }
        dst = (ExEdit::PixelYCA*)((int)dst + line);
    }
}
void mt_color(int thread_id, int thread_num, AlphaControl* ac, ExEdit::FilterProcInfo* efpip) {
    int y = efpip->obj_h * thread_id / thread_num;
    auto dst = (ExEdit::PixelYCA*)efpip->obj_edit + y * efpip->obj_line;
    int line = (efpip->obj_line - efpip->obj_w) * sizeof(ExEdit::PixelYCA);
    for (y = efpip->obj_h * (thread_id + 1) / thread_num - y; 0 < y; y--) {
        for (int x = efpip->obj_w; 0 < x; x--) {
            if (ac->threshold_min <= dst->a && dst->a <= ac->threshold_max) {
                if (dst->a <= 0) {
                    *(int*)dst = *(int*)&ac->y; dst->cr = ac->cr;
                }
                ac->func(dst, ac->v);
            }
            dst++;
        }
        dst = (ExEdit::PixelYCA*)((int)dst + line);
    }
}

BOOL func_proc(ExEdit::Filter* efp, ExEdit::FilterProcInfo* efpip) {
	AlphaControl ac;
	ac.v = max(0, efp->track[0]);
	ac.threshold_max = std::clamp(efp->track[1], 0, 0x1000);
    ac.threshold_min = std::clamp(efp->track[2], 0, 0x1000);
	if (ac.threshold_max < ac.threshold_min) {
        return TRUE;
	}
    Exdata* exdata = reinterpret_cast<decltype(exdata)>(efp->exdata_ptr);
	BOOL flag = ((exdata->color & 0x01000000) == 0 && ac.threshold_min == 0);
	switch (exdata->mode) {
	case 0: // 加算
		if (ac.v == 0) return TRUE;
        (ac.func) = (add);
		break;
	case 1: // 減算
		if (ac.v == 0) return TRUE;
        flag = FALSE;
        (ac.func) = (sub);
		break;
	case 2: // 乗算
		if (ac.v == 0x1000) return TRUE;
        flag = FALSE;
        (ac.func) = (mul);
		break;
	case 3: // スクリーン
		if (ac.v == 0) return TRUE;
        (ac.func) = (screen);
		break;
	case 4: // 上書き
		ac.v = min(efp->track[0], 0x1000);
        (ac.func) = (wrap);
		break;
	}
    if (flag) {
        // rgb2yc(short* y,short* cb,short* cr,int color);
        reinterpret_cast<void(__cdecl*)(short*, short*, short*, int)>(get_exedit92_dll_hinst(efp) + 0x6fed0)(&ac.y, &ac.cb, &ac.cr, exdata->color & 0xffffff);
        efp->aviutl_exfunc->exec_multi_thread_func((AviUtl::MultiThreadFunc)(mt_color), &ac, efpip);
    } else {
        efp->aviutl_exfunc->exec_multi_thread_func((AviUtl::MultiThreadFunc)(mt), &ac, efpip);
	}
	return TRUE;
}

char dlg_color_str[64];
void update_extendedfilter_wnd(ExEdit::Filter* efp) {
    Exdata* exdata = reinterpret_cast<Exdata*>(efp->exdata_ptr);
    if (0 <= exdata->mode || exdata->mode < mode_n) {
        SendMessageA(efp->exfunc->get_hwnd(efp->processing, 6, 0), CB_SETCURSEL, exdata->mode, 0);
    }
    if ((exdata->color & 0x01000000) == 0) {
        wsprintfA(dlg_color_str, "RGB ( %d , %d , %d )", exdata->color & 0xff, (exdata->color >> 8) & 0xff, (exdata->color >> 16) & 0xff);
    } else {
        lstrcpyA(dlg_color_str, "指定無し (元画像の色)");
    }
    SetWindowTextA(efp->exfunc->get_hwnd(efp->processing, 5, 1), dlg_color_str);
}

BOOL func_WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, AviUtl::EditHandle* editp, ExEdit::Filter* efp) {
    if (message == ExEdit::ExtendedFilter::Message::WM_EXTENDEDFILTER_COMMAND) {
        Exdata* exdata = reinterpret_cast<Exdata*>(efp->exdata_ptr);
        if (LOWORD(wparam) == ExEdit::ExtendedFilter::CommandId::EXTENDEDFILTER_SELECT_DROPDOWN) {
            int mode = std::clamp((int)lparam, 0, mode_n - 1);
            if (exdata->mode != mode) {
                efp->exfunc->set_undo(efp->processing, 0);
                exdata->mode = mode;
                // update_any_exdata(ExEdit::ObjectFilterIndex processing, const char* exdata_use_name)
                reinterpret_cast<void(__cdecl*)(ExEdit::ObjectFilterIndex, const char*)>(get_exedit92_dll_hinst(efp) + 0x4a7e0)(efp->processing, exdata_use[0].name);
                update_extendedfilter_wnd(efp);
            }
            return TRUE;
        } else if (LOWORD(wparam) == ExEdit::ExtendedFilter::CommandId::EXTENDEDFILTER_PUSH_BUTTON) {
            efp->exfunc->set_undo(efp->processing, 0);
            if (efp->exfunc->x6c(efp, &exdata->color, 0x102)) { // color_dialog
                // update_any_exdata(ExEdit::ObjectFilterIndex processing, const char* exdata_use_name)
                reinterpret_cast<void(__cdecl*)(ExEdit::ObjectFilterIndex, const char*)>(get_exedit92_dll_hinst(efp) + 0x4a7e0)(efp->processing, exdata_use[1].name);
                reinterpret_cast<void(__cdecl*)(ExEdit::ObjectFilterIndex, const char*)>(get_exedit92_dll_hinst(efp) + 0x4a7e0)(efp->processing, exdata_use[2].name);
                update_extendedfilter_wnd(efp);
            }
            return TRUE;
        }
    }
    return FALSE;
}

int func_window_init(HINSTANCE hinstance, HWND hwnd, int y, int base_id, int sw_param, ExEdit::Filter* efp) {
    update_extendedfilter_wnd(efp);
    return 0;
}


ExEdit::Filter effect_ef = {
    .flag = ExEdit::Filter::Flag::Effect,
    .name = name,
    .track_n = track_n,
    .track_name = track_name,
    .track_default = track_default,
    .track_s = track_s,
    .track_e = track_e,
    .check_n = check_n,
    .check_name = check_name,
    .check_default = check_default,
    .func_proc = &func_proc,
    .func_WndProc = &func_WndProc,
    .exdata_size = exdata_size,
    .func_window_init = &func_window_init,
    .exdata_def = exdata_def,
    .exdata_use = exdata_use,
    .track_scale = track_scale,
    .track_drag_min = track_drag_min,
    .track_drag_max = track_drag_max,
};

ExEdit::Filter* filter_list[] = {
    &effect_ef,
    NULL
};
EXTERN_C __declspec(dllexport)ExEdit::Filter** __stdcall GetFilterTableList() {
    return filter_list;
}
