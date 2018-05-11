//          Copyright Jean Pierre Cimalando 2018.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#if defined(ADLJACK_USE_CURSES)
#include "tui.h"
#include "tui_fileselect.h"
#include "insnames.h"
#include "common.h"
#include <chrono>
#include <cmath>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
namespace stc = std::chrono;

extern std::string get_program_title();

struct TUI_windows
{
    WINDOW_u inner;
    WINDOW_u playertitle;
    WINDOW_u emutitle;
    WINDOW_u chipcount;
    WINDOW_u cpuratio;
    WINDOW_u banktitle;
    WINDOW_u volumeratio;
    WINDOW_u volume[2];
    WINDOW_u instrument[16];
    WINDOW_u status;
    WINDOW_u keydesc1;
    WINDOW_u keydesc2;
};

struct TUI_context
{
    bool quit = false;
    TUI_windows win;
    std::string status_text;
    bool status_display = false;
    unsigned status_timeout = 0;
    stc::steady_clock::time_point status_start;
    Player *player = nullptr;
    std::string bank_directory;
    time_t bank_mtime[player_type_count] = {};
    static constexpr unsigned perc_display_interval = 10;
    unsigned perc_display_cycle = 0;
    bool have_perc_display_program = false;
    Midi_Program_Ex perc_display_program;
};

static void setup_colors();
static void setup_display(TUI_context &ctx);
static void update_display(TUI_context &ctx);
static void show_status(TUI_context &ctx, const std::string &text, unsigned timeout = 10);
static bool handle_anylevel_key(TUI_context &ctx, int key);
static bool handle_toplevel_key(TUI_context &ctx, int key);
static bool update_bank_mtime(TUI_context &ctx);

void curses_interface_exec(void (*idle_proc)(void *), void *idle_data)
{
    Screen screen;
    screen.init();
    if (has_colors())
        setup_colors();
    raw();
    keypad(stdscr, true);
    noecho();
    const unsigned timeout_ms = 50;
    timeout(timeout_ms);
    curs_set(0);
#if !defined(PDCURSES)
    set_escdelay(25);
#endif
#if defined(PDCURSES)
    PDC_set_title(get_program_title().c_str());
#endif

    TUI_context ctx;

    {
        char pathbuf[PATH_MAX + 1];
        if (char *path = getcwd(pathbuf, sizeof(pathbuf)))
            ctx.bank_directory.assign(path);
    }

    setup_display(ctx);
    show_status(ctx, "Ready!");

    unsigned bank_check_interval = 1;
    stc::steady_clock::time_point bank_check_last = stc::steady_clock::now();

    while (!ctx.quit && !interface_interrupted()) {
        if (idle_proc)
            idle_proc(idle_data);

        Player *player = have_active_player() ? &active_player() : nullptr;
        if (ctx.player != player) {
            ctx.player = player;
            update_bank_mtime(ctx);
        }
#if 0
#if defined(PDCURSES)
        bool resized = is_termresized();
#else
        bool resized = is_term_resized(LINES, COLS);
#endif
        if (resized) {
#if defined(PDCURSES)
            resize_term(LINES, COLS);
#else
            resizeterm(LINES, COLS);
#endif
            setup_display(ctx);
            endwin();
            refresh();
        }
#endif

        stc::steady_clock::time_point now = stc::steady_clock::now();
        if (now - bank_check_last > stc::seconds(bank_check_interval)) {
            if (update_bank_mtime(ctx)) {
                if (ctx.player->dynamic_load_bank(active_bank_file().c_str()))
                    show_status(ctx, "Bank has changed on disk. Reload!");
                else
                    show_status(ctx, "Bank has changed on disk. Reloading failed.");
            }
            bank_check_last = now;
        }

        update_display(ctx);

        int key = getch();
        if (!handle_anylevel_key(ctx, key))
            handle_toplevel_key(ctx, key);
    }
    ctx.win = TUI_windows();
    screen.end();

    if (interface_interrupted())
        fprintf(stderr, "Interrupted.\n");
}

static void setup_colors()
{
    start_color();

//#if defined(PDCURSES)
    if (can_change_color()) {
        /* set Tango colors */
        init_color_rgb24(COLOR_BLACK, 0x2e3436);
        init_color_rgb24(COLOR_RED, 0xcc0000);
        init_color_rgb24(COLOR_GREEN, 0x4e9a06);
        init_color_rgb24(COLOR_YELLOW, 0xc4a000);
        init_color_rgb24(COLOR_BLUE, 0x3465a4);
        init_color_rgb24(COLOR_MAGENTA, 0x75507b);
        init_color_rgb24(COLOR_CYAN, 0x06989a);
        init_color_rgb24(COLOR_WHITE, 0xd3d7cf);
        init_color_rgb24(8|COLOR_BLACK, 0x555753);
        init_color_rgb24(8|COLOR_RED, 0xef2929);
        init_color_rgb24(8|COLOR_GREEN, 0x8ae234);
        init_color_rgb24(8|COLOR_YELLOW, 0xf57900);
        init_color_rgb24(8|COLOR_BLUE, 0x729fcf);
        init_color_rgb24(8|COLOR_MAGENTA, 0xad7fa8);
        init_color_rgb24(8|COLOR_CYAN, 0x34e2e2);
        init_color_rgb24(8|COLOR_WHITE, 0xeeeeec);
    }
//#endif

    init_pair(Colors_Background, COLOR_WHITE, COLOR_BLACK);
    init_pair(Colors_Highlight, COLOR_YELLOW, COLOR_BLACK);
    init_pair(Colors_Select, COLOR_BLACK, COLOR_WHITE);
    init_pair(Colors_Frame, COLOR_BLUE, COLOR_BLACK);
    init_pair(Colors_ActiveVolume, COLOR_GREEN, COLOR_BLACK);
    init_pair(Colors_KeyDescription, COLOR_BLACK, COLOR_WHITE);
    init_pair(Colors_Instrument, COLOR_BLUE, COLOR_BLACK);
    init_pair(Colors_ProgramNumber, COLOR_MAGENTA, COLOR_BLACK);
}

static void setup_display(TUI_context &ctx)
{
    ctx.win = TUI_windows();

    bkgd(COLOR_PAIR(Colors_Background));

    WINDOW *inner = subwin(stdscr, LINES - 2, COLS - 2, 1, 1);
    if (!inner) return;
    ctx.win.inner.reset(inner);

    unsigned cols = getcols(inner);
    unsigned rows = getrows(inner);

    int row = 0;

    ctx.win.playertitle = linewin(inner, row++, 0);
    ctx.win.emutitle = linewin(inner, row++, 0);
    ctx.win.chipcount = linewin(inner, row++, 0);
    ctx.win.cpuratio = linewin(inner, row++, 0);
    ctx.win.banktitle = linewin(inner, row++, 0);
    ctx.win.volumeratio = linewin(inner, row++, 0);

    for (unsigned channel = 0; channel < 2; ++channel)
        ctx.win.volume[channel] = linewin(inner, row++, 0);
    ++row;

    for (unsigned midichannel = 0; midichannel < 16; ++midichannel) {
        unsigned width = cols / 2;
        unsigned row2 = row + midichannel % 8;
        unsigned col = (midichannel < 8) ? 0 : width;
        ctx.win.instrument[midichannel].reset(derwin(inner, 1, width, row2, col));
    }
    row += 8;

    ctx.win.status.reset(derwin(inner, 1, cols, rows - 4, 0));
    ctx.win.keydesc1.reset(derwin(inner, 1, cols, rows - 2, 0));
    ctx.win.keydesc2.reset(derwin(inner, 1, cols, rows - 1, 0));
}

static void print_bar(WINDOW *w, double vol, char ch_on, char ch_off, int attr_on)
{
    unsigned size = getcols(w);
    if (size < 2)
        return;
    mvwaddch(w, 0, 0, '[');
    for (unsigned i = 0; i < size - 2; ++i) {
        double ref = (double)i / (size - 2);
        bool gt = vol > ref;
        if (gt) wattron(w, attr_on);
        mvwaddch(w, 0, i + 1, gt ? ch_on : ch_off);
        if (gt) wattroff(w, attr_on);
    }
    mvwaddch(w, 0, size - 1, ']');
}

static void update_display(TUI_context &ctx)
{
    Player *player = ctx.player;

    {
        std::string title = get_program_title();
        size_t titlesize = title.size();

        attron(A_BOLD|COLOR_PAIR(Colors_Frame));
        //border(' ', ' ', '-', ' ', '-', '-', ' ', ' ');
        border(' ', ' ', '-', '-', '-', '-', '-', '-');
        attroff(A_BOLD|COLOR_PAIR(Colors_Frame));

        unsigned cols = getcols(stdscr);
        if (cols >= titlesize + 2) {
            unsigned x = (cols - (titlesize + 2)) / 2;
            attron(A_BOLD|COLOR_PAIR(Colors_Frame));
            mvaddch(0, x, '(');
            mvaddch(0, x + titlesize + 1, ')');
            attroff(A_BOLD|COLOR_PAIR(Colors_Frame));
            mvaddstr(0, x + 1, title.c_str());
        }
    }

    if (WINDOW *w = ctx.win.playertitle.get()) {
        wclear(w);
        mvwaddstr(w, 0, 0, "Player");
        if (player) {
            wattron(w, COLOR_PAIR(Colors_Highlight));
            mvwprintw(w, 0, 10, "%s %s", player->name(), player->version());
            wattroff(w, COLOR_PAIR(Colors_Highlight));
        }
    }
    if (WINDOW *w = ctx.win.emutitle.get()) {
        wclear(w);
        mvwaddstr(w, 0, 0, "Emulator");
        if (player) {
            wattron(w, COLOR_PAIR(Colors_Highlight));
            mvwaddstr(w, 0, 10, player->emulator_name());
            wattroff(w, COLOR_PAIR(Colors_Highlight));
        }
    }
    if (WINDOW *w = ctx.win.chipcount.get()) {
        wclear(w);
        mvwaddstr(w, 0, 0, "Chips");
        if (player) {
            wattron(w, COLOR_PAIR(Colors_Highlight));
            mvwprintw(w, 0, 10, "%u", player->chip_count());
            wattroff(w, COLOR_PAIR(Colors_Highlight));
            waddstr(w, " * ");
            wattron(w, COLOR_PAIR(Colors_Highlight));
            waddstr(w, player->chip_name());
            wattroff(w, COLOR_PAIR(Colors_Highlight));
        }
    }
    if (WINDOW *w = ctx.win.cpuratio.get()) {
        wclear(w);
        mvwaddstr(w, 0, 0, "CPU");

        WINDOW_u barw(derwin(w, 1, 25, 0, 10));
        if (barw)
            print_bar(barw.get(), cpuratio, '*', '-', COLOR_PAIR(Colors_Highlight));
    }
    if (WINDOW *w = ctx.win.banktitle.get()) {
        wclear(w);
        mvwaddstr(w, 0, 0, "Bank");
        if (player) {
            std::string title;
            const std::string &path = active_bank_file();
            if (path.empty())
                title = "(default)";
            else
            {
#if !defined(_WIN32)
                size_t pos = path.rfind('/');
#else
                size_t pos = path.find_last_of("/\\");
#endif
                title = (pos != path.npos) ? path.substr(pos + 1) : path;
            }
            wattron(w, COLOR_PAIR(Colors_Highlight));
            mvwaddstr(w, 0, 10, title.c_str());
            wattroff(w, COLOR_PAIR(Colors_Highlight));
        }
    }

    double channel_volumes[2] = {lvcurrent[0], lvcurrent[1]};
    const char *channel_names[2] = {"Left", "Right"};

    // enables logarithmic view for perceptual volume, otherwise linear.
    //  (better use linear to watch output for clipping)
    const bool logarithmic = false;

    if (WINDOW *w = ctx.win.volumeratio.get()) {
        mvwaddstr(w, 0, 0, "Volume");
        wattron(w, COLOR_PAIR(Colors_Highlight));
        mvwprintw(w, 0, 10, "%3d%%\n", ::player_volume);
        wattroff(w, COLOR_PAIR(Colors_Highlight));
    }

    for (unsigned channel = 0; channel < 2; ++channel) {
        WINDOW *w = ctx.win.volume[channel].get();
        if (!w) continue;

        double vol = channel_volumes[channel];
        if (logarithmic && vol > 0) {
            double db = 20 * log10(vol);
            const double dbmin = -60.0;
            vol = (db - dbmin) / (0 - dbmin);
        }

        mvwaddstr(w, 0, 0, channel_names[channel]);

        WINDOW_u barw(derwin(w, 1, getcols(w) - 6, 0, 6));
        if (barw)
            print_bar(barw.get(), vol, '*', '-', A_BOLD|COLOR_PAIR(Colors_ActiveVolume));

        wrefresh(w);
    }

    for (unsigned midichannel = 0; midichannel < 16; ++midichannel) {
        WINDOW *w = ctx.win.instrument[midichannel].get();
        if (!w) continue;
        wclear(w);
        const Program &pgm = channel_map[midichannel];
        mvwprintw(w, 0, 0, "%2u: [", midichannel + 1);
        wattron(w, A_BOLD|COLOR_PAIR(Colors_ProgramNumber));
        wprintw(w, "%3u", pgm.gm);
        wattroff(w, A_BOLD|COLOR_PAIR(Colors_ProgramNumber));
        waddstr(w, "]");
        if (midi_channel_note_count[midichannel] > 0) {
            wattron(w, A_BOLD|COLOR_PAIR(Colors_ActiveVolume));
            mvwaddch(w, 0, 11, '*');
            wattroff(w, A_BOLD|COLOR_PAIR(Colors_ActiveVolume));
        }

        const char *name = nullptr;
        Midi_Spec spec = Midi_Spec::GM;

        const int ms_attr[5] = {
            A_BOLD|COLOR_PAIR(Colors_Instrument),
            A_BOLD|COLOR_PAIR(Colors_InstrumentEx),
            A_BOLD|COLOR_PAIR(Colors_InstrumentEx),
            A_BOLD|COLOR_PAIR(Colors_InstrumentEx),
            A_BOLD|COLOR_PAIR(Colors_InstrumentEx),
        };

        if (midichannel == 9) {
            // percussion display, with update rate limit
            if (++ctx.perc_display_cycle == ctx.perc_display_interval) {
                ctx.perc_display_cycle = 0;
                if (unsigned pgm = ::midi_channel_last_note_p1[midichannel]) {
                    --pgm;
                    ctx.have_perc_display_program = true;
                    ctx.perc_display_program = midi_percussion[pgm];
                }
            }
            if (!ctx.have_perc_display_program)
                name = "Percussion";
            else {
                spec = ctx.perc_display_program.spec;
                name = ctx.perc_display_program.name;
            }
        }
        else {
            // melodic display
            if (pgm.bank_msb | pgm.bank_lsb) {
                if (const Midi_Program_Ex *ex = midi_program_ex_find(
                        pgm.bank_msb, pgm.bank_lsb, pgm.gm)) {
                    spec = ex->spec;
                    name = ex->name;
                }
            }
            if (!name)
                name = midi_instrument_name[pgm.gm];
        }

        int attr = ms_attr[(unsigned)spec];
        wattron(w, attr);
        mvwaddstr(w, 0, 12, name);
        wattroff(w, attr);
        if (spec != Midi_Spec::GM)
            wprintw(w, " [%s]", midi_spec_name(spec));
    }

    if (WINDOW *w = ctx.win.status.get()) {
        if (!ctx.status_text.empty()) {
            if (!ctx.status_display) {
                wclear(w);
                wattron(w, COLOR_PAIR(Colors_Select));
                waddstr(w, ctx.status_text.c_str());
                wattroff(w, COLOR_PAIR(Colors_Select));
                ctx.status_start = stc::steady_clock::now();
                ctx.status_display = true;
            }
            else if (stc::steady_clock::now() - ctx.status_start > stc::seconds(ctx.status_timeout)) {
                wclear(w);
                ctx.status_text.clear();
                ctx.status_display = false;
            }
        }
    }

    struct Key_Description {
        Key_Description(const char *key, const char *desc)
            : key(key), desc(desc) {}
        const char *key = nullptr;
        const char *desc = nullptr;
    };

    const unsigned key_spacing = 16;

    if (WINDOW *w = ctx.win.keydesc1.get()) {
        wclear(w);

        static const Key_Description keydesc[] = {
            { "<", "prev emulator" },
            { ">", "next emulator" },
            { "[", "chips -1" },
            { "]", "chips +1" },
            { "b", "load bank" },
        };
        unsigned nkeydesc = sizeof(keydesc) / sizeof(*keydesc);

        for (unsigned i = 0; i < nkeydesc; ++i) {
            wmove(w, 0, i * key_spacing);
            wattron(w, COLOR_PAIR(Colors_KeyDescription));
            waddstr(w, keydesc[i].key);
            wattroff(w, COLOR_PAIR(Colors_KeyDescription));
            waddstr(w, " ");
            waddstr(w, keydesc[i].desc);
        }
    }

    if (WINDOW *w = ctx.win.keydesc2.get()) {
        wclear(w);

        static const Key_Description keydesc[] = {
            { "/", "volume -1" },
            { "*", "volume +1" },
            { "p", "panic" },
            { "q", "quit" },
        };
        unsigned nkeydesc = sizeof(keydesc) / sizeof(*keydesc);

        for (unsigned i = 0; i < nkeydesc; ++i) {
            wmove(w, 0, i * key_spacing);
            wattron(w, COLOR_PAIR(Colors_KeyDescription));
            waddstr(w, keydesc[i].key);
            wattroff(w, COLOR_PAIR(Colors_KeyDescription));
            waddstr(w, " ");
            waddstr(w, keydesc[i].desc);
        }
    }
}

static void show_status(TUI_context &ctx, const std::string &text, unsigned timeout)
{
    ctx.status_text = text;
    ctx.status_display = false;
    ctx.status_timeout = timeout;
}

static bool handle_anylevel_key(TUI_context &ctx, int key)
{
    switch (key) {
    default:
        return false;
    case 'q': case 'Q':
    case 3:   // console break
        ctx.quit = true;
        return true;;
#if 0
    case KEY_RESIZE:
        clear();
        return true;
#endif
    }
}

static bool handle_toplevel_key(TUI_context &ctx, int key)
{
    Player *player = ctx.player;
    if (!player)
        return false;

    switch (key) {
    default:
        return false;
    case '<': {
        if (active_emulator_id > 0)
            dynamic_switch_emulator_id(active_emulator_id - 1);
        return true;
    }
    case '>': {
        if (active_emulator_id + 1 < emulator_ids.size())
            dynamic_switch_emulator_id(active_emulator_id + 1);
        return true;
    }
    case '[': {
        unsigned nchips = player->chip_count();
        if (nchips > 1)
            player->dynamic_set_chip_count(nchips - 1);
        return true;
    }
    case ']': {
        unsigned nchips = player->chip_count();
        player->dynamic_set_chip_count(nchips + 1);
        return true;
    }
    case '/': {
        ::player_volume = std::max(volume_min, ::player_volume - 1);
        return true;
    }
    case '*': {
        ::player_volume = std::min(volume_max, ::player_volume + 1);
        return true;
    }
    case 'b':
    case 'B': {
        WINDOW_u w(newwin(getrows(stdscr), getcols(stdscr), 0, 0));
        if (w) {
            File_Selection_Options fopts;
            fopts.title = "Load bank";
            fopts.directory = ctx.bank_directory;
            File_Selector fs(w.get(), fopts);
            File_Selection_Code code = File_Selection_Code::Continue;
            fs.update();
            for (key = getch(); !ctx.quit && !interface_interrupted() &&
                     code == File_Selection_Code::Continue; key = getch()) {
                if (!handle_anylevel_key(ctx, key)) {
                    code = fs.key(key);
                    fs.update();
                }
            }
            if (code == File_Selection_Code::Ok) {
                if (player->dynamic_load_bank(fopts.filepath.c_str())) {
                    show_status(ctx, "Bank loaded!");
                    active_bank_file() = fopts.filepath;
                    update_bank_mtime(ctx);
                }
                else
                    show_status(ctx, "Error loading the bank file.");
                ctx.bank_directory = fopts.directory;
            }
            clear();
        }
        return true;
    }
    case 'p':
    case 'P': {
        player->dynamic_panic();
        return true;
    }
    }
}

static bool update_bank_mtime(TUI_context &ctx)
{
    Player *player = ctx.player;
    if (!player)
        return false;
    struct stat st;
    const char *path = active_bank_file().c_str();
    Player_Type pt = player->type();
    time_t old_mtime = ctx.bank_mtime[(unsigned)pt];
    time_t new_mtime = (path[0] && !stat(path, &st)) ? st.st_mtime : 0;
    ctx.bank_mtime[(unsigned)pt] = new_mtime;
    return new_mtime && new_mtime != old_mtime;
}

//------------------------------------------------------------------------------
int getrows(WINDOW *w)
{
    return getmaxy(w) - getbegy(w);
}

int getcols(WINDOW *w)
{
    return getmaxx(w) - getbegx(w);
}

WINDOW_u linewin(WINDOW *w, int row, int col)
{
    return WINDOW_u(derwin(w, 1, getcols(w) - col, row, col));
}

int init_color_rgb24(short id, uint32_t value)
{
    unsigned r = (value >> 16) & 0xff;
    unsigned g = (value >> 8) & 0xff;
    unsigned b = value & 0xff;
    return init_color(id, r * 1000 / 0xff, g * 1000 / 0xff, b * 1000 / 0xff);
}

#endif  // defined(ADLJACK_USE_CURSES)
