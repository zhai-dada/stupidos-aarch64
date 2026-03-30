#include "stupidos_user.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*
 * /bin/ui
 *
 * 这是一个真正意义上的“用户态界面入口”：
 * - framebuffer 可用时，绘制一个简洁的桌面面板
 * - framebuffer 不可用时，退化成终端版状态页
 * - 通过 q / r / Enter 做最小交互
 *
 * 这样我们就不是只有内核里的 boot splash，而是拥有一个可单独启动的 UI ELF。
 */

#define UI_BAR_H            48U
#define UI_MARGIN           24U
#define UI_CARD_GAP         16U
#define UI_FOOTER_H         40U
#define UI_REFRESH_MS       16
#define UI_FILE_MAX         12
#define UI_BUTTON_ROWS      2
#define UI_BUTTON_COLS      3

#define UI_BG               0x00101820
#define UI_PANEL            0x0018222D
#define UI_PANEL_DARK       0x0012161C
#define UI_ACCENT           0x00FFB000
#define UI_ACCENT2          0x0000D0FF
#define UI_OK               0x0000FF88
#define UI_WARN             0x00FF4040
#define UI_WHITE            0x00FFFFFF

enum ui_action
{
    UI_ACTION_NONE = 0,
    UI_ACTION_SHELL,
    UI_ACTION_BROWSER,
    UI_ACTION_FILES,
    UI_ACTION_NETWORK,
    UI_ACTION_REFRESH,
    UI_ACTION_QUIT,
};

struct ui_button
{
    const char *label;
    const char *hint;
    enum ui_action action;
    uint32_t x;
    uint32_t y;
    uint32_t w;
    uint32_t h;
};

struct ui_file_item
{
    char name[STUPIDOS_PATH_MAX];
    uint32_t mode;
    uint64_t size;
    bool valid;
};

struct ui_state
{
    struct stupidos_fbinfo fb;
    struct stupidos_mouseinfo mouse;
    struct stupidos_mouseinfo prev_mouse;
    bool have_fb;
    bool raw_enabled;
    struct termios saved_termios;
    bool saved_termios_valid;
    bool mouse_valid;
    bool show_files;
    bool show_network;
    char cwd[STUPIDOS_PATH_MAX];
    char release[64];
    char machine[32];
    char status[160];
    struct ui_button buttons[UI_BUTTON_ROWS * UI_BUTTON_COLS];
    struct ui_file_item files[UI_FILE_MAX];
    uint32_t file_count;
    uint32_t hovered_button;
    uint32_t clicked_button;
};

static struct ui_state ui_global;

static void ui_puts(const int8_t *s)
{
    if (!s)
    {
        return;
    }
    (void)u_write(STUPIDOS_STDOUT_FILENO, s, u_strlen(s));
}

static void ui_restore_terminal(struct ui_state *ui)
{
    if (!ui || !ui->raw_enabled)
    {
        return;
    }

    if (ui->saved_termios_valid)
    {
        (void)tcsetattr(STUPIDOS_STDIN_FILENO, TCSANOW, &ui->saved_termios);
    }
    (void)u_write(STUPIDOS_STDOUT_FILENO, (const int8_t *)"\x1b[?25h\x1b[0m\r\n", 12);
    ui->raw_enabled = false;
}

static void ui_restore_terminal_atexit(void)
{
    ui_restore_terminal(&ui_global);
}

static void ui_enable_raw_terminal(struct ui_state *ui)
{
    struct termios raw;

    if (!ui || !isatty(STUPIDOS_STDIN_FILENO))
    {
        return;
    }

    if (tcgetattr(STUPIDOS_STDIN_FILENO, &ui->saved_termios) < 0)
    {
        return;
    }

    raw = ui->saved_termios;
    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON | IXOFF);
    raw.c_oflag &= (tcflag_t)~OPOST;
    raw.c_cflag |= (tcflag_t)(CS8 | CREAD);
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STUPIDOS_STDIN_FILENO, TCSANOW, &raw) == 0)
    {
        ui->raw_enabled = true;
        ui->saved_termios_valid = true;
        (void)u_write(STUPIDOS_STDOUT_FILENO, (const int8_t *)"\x1b[?25l", 6);
    }
}

static void ui_clear_terminal(void)
{
    ui_puts((const int8_t *)"\x1b[2J\x1b[H");
}

static void ui_color_fill(struct ui_state *ui, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    if (!ui || !ui->have_fb)
    {
        return;
    }
    (void)u_fbfill(x, y, w, h, color);
}

static void ui_color_text(struct ui_state *ui, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg, const void *s)
{
    if (!ui || !s)
    {
        return;
    }
    if (ui->have_fb)
    {
        (void)u_fbtext(x, y, fg, bg, (const int8_t *)s);
    }
    else
    {
        ui_puts((const int8_t *)s);
        ui_puts((const int8_t *)"\r\n");
    }
}

static uint32_t ui_clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    if (v < lo)
    {
        return lo;
    }
    if (v > hi)
    {
        return hi;
    }
    return v;
}

static bool ui_point_in_rect(uint32_t px, uint32_t py, const struct ui_button *btn)
{
    if (!btn)
    {
        return false;
    }

    return px >= btn->x && px < (btn->x + btn->w) &&
           py >= btn->y && py < (btn->y + btn->h);
}

static void ui_copy_cwd(struct ui_state *ui)
{
    int8_t buf[STUPIDOS_PATH_MAX];

    if (!ui)
    {
        return;
    }

    u_memset(ui->cwd, 0, sizeof(ui->cwd));
    if (u_getcwd(buf, sizeof(buf)) >= 0 && buf[0] != '\0')
    {
        u_memcpy(ui->cwd, buf, u_strnlen(buf, sizeof(ui->cwd) - 1U));
        return;
    }

    u_memcpy(ui->cwd, (const int8_t *)"/", 2);
}

static void ui_set_status(struct ui_state *ui, const char *msg)
{
    if (!ui)
    {
        return;
    }

    if (!msg)
    {
        ui->status[0] = '\0';
        return;
    }

    (void)snprintf(ui->status, sizeof(ui->status), "%s", msg);
}

static void ui_launch_child(struct ui_state *ui, const char *path, const char *arg0, const char *label)
{
    const int8_t *argv[2];
    int pid;
    int status;
    char msg[160];

    if (!ui || !path)
    {
        return;
    }

    argv[0] = (const int8_t *)(arg0 ? arg0 : path);
    argv[1] = NULL;
    pid = u_exec((const int8_t *)path, 1, argv);
    if (pid < 0)
    {
        (void)snprintf(msg, sizeof(msg), "%s launch failed (%d)", label ? label : path, pid);
        ui_set_status(ui, msg);
        return;
    }

    (void)snprintf(msg, sizeof(msg), "%s running as pid %d", label ? label : path, pid);
    ui_set_status(ui, msg);
    (void)u_waitpid_status((int32_t)pid, &status);
    (void)snprintf(msg, sizeof(msg), "%s exited status %d", label ? label : path, status);
    ui_set_status(ui, msg);
}

static void ui_refresh_files(struct ui_state *ui)
{
    struct stupidos_dirent ent;
    uint32_t index;
    uint32_t count;
    int ret;

    if (!ui)
    {
        return;
    }

    ui->file_count = 0;
    for (count = 0; count < UI_FILE_MAX; count++)
    {
        ui->files[count].valid = false;
        ui->files[count].name[0] = '\0';
        ui->files[count].mode = 0;
        ui->files[count].size = 0;
    }

    index = 0;
    while (ui->file_count < UI_FILE_MAX)
    {
        ret = u_readdir(ui->cwd[0] ? (const int8_t *)ui->cwd : (const int8_t *)"/", index, &ent);
        if (ret == -STUPIDOS_ENOENT)
        {
            break;
        }
        if (ret < 0)
        {
            ui_set_status(ui, "directory read failed");
            break;
        }

        if (ent.name[0] == '\0')
        {
            index++;
            continue;
        }

        ui->files[ui->file_count].valid = true;
        ui->files[ui->file_count].mode = ent.mode;
        ui->files[ui->file_count].size = ent.size;
        (void)snprintf(ui->files[ui->file_count].name, sizeof(ui->files[ui->file_count].name), "%s", ent.name);
        ui->file_count++;
        index++;
    }
}

static void ui_layout_buttons(struct ui_state *ui)
{
    uint32_t card_w;
    uint32_t card_h;
    uint32_t gap_x;
    uint32_t gap_y;
    uint32_t start_x;
    uint32_t start_y;
    uint32_t i;

    if (!ui || !ui->have_fb)
    {
        return;
    }

    if (ui->fb.width <= (UI_MARGIN * 2U) + (UI_CARD_GAP * (UI_BUTTON_COLS - 1U)) + UI_BUTTON_COLS)
    {
        return;
    }

    card_w = (ui->fb.width - (UI_MARGIN * 2U) - (UI_CARD_GAP * (UI_BUTTON_COLS - 1U))) / UI_BUTTON_COLS;
    card_h = 76U;
    gap_x = UI_CARD_GAP;
    gap_y = UI_CARD_GAP;
    start_x = UI_MARGIN;
    start_y = UI_BAR_H + 18U;

    for (i = 0; i < (uint32_t)(UI_BUTTON_ROWS * UI_BUTTON_COLS); i++)
    {
        uint32_t row = i / UI_BUTTON_COLS;
        uint32_t col = i % UI_BUTTON_COLS;
        struct ui_button *btn = &ui->buttons[i];

        btn->x = start_x + col * (card_w + gap_x);
        btn->y = start_y + row * (card_h + gap_y);
        btn->w = card_w;
        btn->h = card_h;
    }
}

static void ui_init_buttons(struct ui_state *ui)
{
    if (!ui)
    {
        return;
    }

    ui->buttons[0].label = "Shell";
    ui->buttons[0].hint = "launch /bin/sh";
    ui->buttons[0].action = UI_ACTION_SHELL;

    ui->buttons[1].label = "Browser";
    ui->buttons[1].hint = "launch browser";
    ui->buttons[1].action = UI_ACTION_BROWSER;

    ui->buttons[2].label = "Files";
    ui->buttons[2].hint = "toggle file panel";
    ui->buttons[2].action = UI_ACTION_FILES;

    ui->buttons[3].label = "Network";
    ui->buttons[3].hint = "show network status";
    ui->buttons[3].action = UI_ACTION_NETWORK;

    ui->buttons[4].label = "Refresh";
    ui->buttons[4].hint = "redraw desktop";
    ui->buttons[4].action = UI_ACTION_REFRESH;

    ui->buttons[5].label = "Quit";
    ui->buttons[5].hint = "exit UI";
    ui->buttons[5].action = UI_ACTION_QUIT;

    ui->hovered_button = 0;
    ui->clicked_button = 0;
}

static void ui_draw_button(struct ui_state *ui, const struct ui_button *btn, bool hovered)
{
    uint32_t body;
    uint32_t title_fg;
    uint32_t hint_fg;

    if (!ui || !ui->have_fb || !btn)
    {
        return;
    }

    body = hovered ? 0x0022364A : UI_PANEL;
    title_fg = hovered ? UI_WHITE : UI_ACCENT;
    hint_fg = hovered ? UI_OK : UI_WHITE;
    ui_color_fill(ui, btn->x, btn->y, btn->w, btn->h, body);
    ui_color_fill(ui, btn->x, btn->y, btn->w, 3, hovered ? UI_ACCENT2 : 0x004B6074);
    ui_color_text(ui, btn->x + 12U, btn->y + 12U, title_fg, body, btn->label);
    ui_color_text(ui, btn->x + 12U, btn->y + 36U, hint_fg, body, btn->hint);
}

static void ui_draw_file_panel(struct ui_state *ui)
{
    uint32_t panel_x;
    uint32_t panel_y;
    uint32_t panel_w;
    uint32_t panel_h;
    uint32_t i;
    uint32_t line_y;
    char title[128];
    char line[128];

    if (!ui || !ui->have_fb)
    {
        return;
    }

    panel_x = UI_MARGIN;
    panel_y = UI_BAR_H + 190U;
    panel_w = (ui->fb.width / 2U) - (UI_MARGIN * 1U);
    if (ui->fb.height > panel_y + UI_FOOTER_H + 24U)
    {
        panel_h = ui->fb.height - panel_y - UI_FOOTER_H - 24U;
    }
    else
    {
        panel_h = 0U;
    }

    ui_color_fill(ui, panel_x, panel_y, panel_w, panel_h, UI_PANEL_DARK);
    ui_color_fill(ui, panel_x, panel_y, panel_w, 3, UI_OK);

    (void)snprintf(title, sizeof(title), "files  cwd: %s", ui->cwd[0] ? ui->cwd : "/");
    ui_color_text(ui, panel_x + 12U, panel_y + 12U, UI_WHITE, UI_PANEL_DARK, title);
    ui_color_text(ui, panel_x + 12U, panel_y + 34U, UI_WHITE, UI_PANEL_DARK,
                  "click directory to enter, click file to exec");

    line_y = panel_y + 64U;
    for (i = 0; i < ui->file_count; i++)
    {
        const struct ui_file_item *it = &ui->files[i];
        const char *type;

        if (!it->valid)
        {
            continue;
        }

        if ((it->mode & STUPIDOS_VFS_S_IFMT) == STUPIDOS_VFS_S_IFDIR)
        {
            type = "[dir]";
        }
        else if ((it->mode & STUPIDOS_VFS_S_IFMT) == STUPIDOS_VFS_S_IFLNK)
        {
            type = "[lnk]";
        }
        else
        {
            type = "[file]";
        }

        (void)snprintf(line, sizeof(line), "%s  %s  %lu", type, it->name, (unsigned long)it->size);
        ui_color_text(ui, panel_x + 16U, line_y, UI_WHITE, UI_PANEL_DARK, line);
        line_y += 22U;
        if (line_y + 24U >= panel_y + panel_h)
        {
            break;
        }
    }
}

static void ui_draw_network_panel(struct ui_state *ui)
{
    struct stupidos_utsname uname_info;
    struct stupidos_sysinfo sys;
    char line[160];
    uint32_t panel_x;
    uint32_t panel_y;
    uint32_t panel_w;
    uint32_t panel_h;

    if (!ui || !ui->have_fb)
    {
        return;
    }

    panel_x = (ui->fb.width / 2U) + 8U;
    panel_y = UI_BAR_H + 190U;
    panel_w = ui->fb.width - panel_x - UI_MARGIN;
    if (ui->fb.height > panel_y + UI_FOOTER_H + 24U)
    {
        panel_h = ui->fb.height - panel_y - UI_FOOTER_H - 24U;
    }
    else
    {
        panel_h = 0U;
    }

    memset(&uname_info, 0, sizeof(uname_info));
    memset(&sys, 0, sizeof(sys));
    (void)u_uname(&uname_info);
    (void)u_sysinfo(&sys);

    ui_color_fill(ui, panel_x, panel_y, panel_w, panel_h, UI_PANEL);
    ui_color_fill(ui, panel_x, panel_y, panel_w, 3, UI_ACCENT2);
    ui_color_text(ui, panel_x + 12U, panel_y + 12U, UI_WHITE, UI_PANEL, "system");
    (void)snprintf(line, sizeof(line), "%s %s", uname_info.sysname, uname_info.release);
    ui_color_text(ui, panel_x + 12U, panel_y + 36U, UI_WHITE, UI_PANEL, line);
    (void)snprintf(line, sizeof(line), "cpu %s  machine %s", uname_info.version, uname_info.machine);
    ui_color_text(ui, panel_x + 12U, panel_y + 58U, UI_WHITE, UI_PANEL, line);
    (void)snprintf(line, sizeof(line), "mem free %lu MB / %lu MB",
                   (unsigned long)(sys.freeram / 1024ULL / 1024ULL),
                   (unsigned long)(sys.totalram / 1024ULL / 1024ULL));
    ui_color_text(ui, panel_x + 12U, panel_y + 80U, UI_WHITE, UI_PANEL, line);
    (void)snprintf(line, sizeof(line), "status: %s", ui->status[0] ? ui->status : "ready");
    ui_color_text(ui, panel_x + 12U, panel_y + 102U, UI_WHITE, UI_PANEL, line);
    ui_color_text(ui, panel_x + 12U, panel_y + 124U, UI_WHITE, UI_PANEL,
                  "mouse click buttons, scroll files, enter to refresh");
}

static void ui_draw_cursor(struct ui_state *ui, uint32_t x, uint32_t y)
{
    uint32_t cx;
    uint32_t cy;

    if (!ui || !ui->have_fb)
    {
        return;
    }

    if (ui->fb.width > 9U)
    {
        cx = ui_clamp_u32(x, 4U, ui->fb.width - 5U);
    }
    else
    {
        cx = 0U;
    }

    if (ui->fb.height > 9U)
    {
        cy = ui_clamp_u32(y, 4U, ui->fb.height - 5U);
    }
    else
    {
        cy = 0U;
    }

    ui_color_fill(ui, cx, cy, 9U, 1U, UI_WHITE);
    ui_color_fill(ui, cx + 4U, cy - 4U, 1U, 9U, UI_WHITE);
    ui_color_fill(ui, cx + 1U, cy + 1U, 3U, 3U, UI_ACCENT);
}

static enum ui_action ui_hit_test_button(struct ui_state *ui, uint32_t x, uint32_t y, uint32_t *index_out)
{
    uint32_t i;

    if (!ui)
    {
        return UI_ACTION_NONE;
    }

    for (i = 0; i < (uint32_t)(UI_BUTTON_ROWS * UI_BUTTON_COLS); i++)
    {
        if (ui_point_in_rect(x, y, &ui->buttons[i]))
        {
            if (index_out)
            {
                *index_out = i;
            }
            return ui->buttons[i].action;
        }
    }

    if (index_out)
    {
        *index_out = 0;
    }
    return UI_ACTION_NONE;
}

static void ui_open_path_or_exec(struct ui_state *ui, const char *name, uint32_t mode)
{
    char full[STUPIDOS_PATH_MAX * 2];
    const int8_t *argv[2];
    int pid;
    int status;

    if (!ui || !name || name[0] == '\0')
    {
        return;
    }

    if (ui->cwd[0] == '\0' || strcmp(ui->cwd, "/") == 0)
    {
        (void)snprintf(full, sizeof(full), "/%s", name);
    }
    else if (ui->cwd[strlen(ui->cwd) - 1U] == '/')
    {
        (void)snprintf(full, sizeof(full), "%s%s", ui->cwd, name);
    }
    else
    {
        (void)snprintf(full, sizeof(full), "%s/%s", ui->cwd, name);
    }

    if ((mode & STUPIDOS_VFS_S_IFMT) == STUPIDOS_VFS_S_IFDIR)
    {
        if (u_chdir((const int8_t *)full) == 0)
        {
            ui_copy_cwd(ui);
            ui_refresh_files(ui);
            ui_set_status(ui, "directory changed");
        }
        else
        {
            ui_set_status(ui, "chdir failed");
        }
        return;
    }

    argv[0] = (const int8_t *)full;
    argv[1] = NULL;
    pid = u_exec((const int8_t *)full, 1, argv);
    if (pid < 0)
    {
        ui_set_status(ui, "exec failed");
        return;
    }

    (void)u_waitpid_status((int32_t)pid, &status);
    (void)snprintf(ui->status, sizeof(ui->status), "%s exited status %d", name, status);
}

static void ui_open_file_item(struct ui_state *ui, const struct ui_file_item *it)
{
    if (!ui || !it || !it->valid)
    {
        return;
    }

    ui_open_path_or_exec(ui, it->name, it->mode);
}


static void ui_draw_terminal_view(struct ui_state *ui, const char *message)
{
    struct stupidos_timeval tv;
    struct stupidos_sysinfo sys;
    struct stupidos_utsname uname_info;
    uint64_t uptime_s;
    uint64_t h;
    uint64_t m;
    uint64_t s;
    char buf[128];

    (void)u_gettimeofday(&tv);
    (void)u_sysinfo(&sys);
    (void)u_uname(&uname_info);

    uptime_s = (tv.tv_sec > 0) ? (uint64_t)tv.tv_sec : 0ULL;
    h = uptime_s / 3600ULL;
    m = (uptime_s % 3600ULL) / 60ULL;
    s = uptime_s % 60ULL;

    ui_clear_terminal();
    ui_puts((const int8_t *)"stupidos ui\n");
    ui_puts((const int8_t *)"------------------------------\n");
    (void)snprintf(buf, sizeof(buf), "host: %s %s\n", uname_info.nodename, uname_info.machine);
    ui_puts((const int8_t *)buf);
    (void)snprintf(buf, sizeof(buf), "uptime: %02lu:%02lu:%02lu\n", (unsigned long)h, (unsigned long)m, (unsigned long)s);
    ui_puts((const int8_t *)buf);
    (void)snprintf(buf, sizeof(buf), "mem: %lu/%lu MB free\n",
                   (unsigned long)(sys.freeram / 1024ULL / 1024ULL),
                   (unsigned long)(sys.totalram / 1024ULL / 1024ULL));
    ui_puts((const int8_t *)buf);
    (void)snprintf(buf, sizeof(buf), "cwd: %s\n", ui->cwd[0] ? ui->cwd : "/");
    ui_puts((const int8_t *)buf);
    (void)snprintf(buf, sizeof(buf), "mouse: x=%d y=%d buttons=%#x\n",
                   ui->mouse.x, ui->mouse.y, ui->mouse.buttons);
    ui_puts((const int8_t *)buf);
    ui_puts((const int8_t *)"keys: q quit | r refresh | Enter redraw\n");
    if (message)
    {
        ui_puts((const int8_t *)message);
        ui_puts((const int8_t *)"\n");
    }
}

static void ui_draw_frame(struct ui_state *ui)
{
    struct stupidos_timeval tv;
    struct stupidos_sysinfo sys;
    struct stupidos_utsname uname_info;
    char uptime_buf[64];
    char mem_buf[64];
    char cpu_buf[64];
    char info_buf[128];
    uint64_t uptime_s;
    uint64_t h;
    uint64_t m;
    uint64_t s;
    uint32_t i;
    uint32_t hovered;
    bool show_hover_info;

    if (!ui || !ui->have_fb)
    {
        return;
    }

    memset(&tv, 0, sizeof(tv));
    memset(&sys, 0, sizeof(sys));
    memset(&uname_info, 0, sizeof(uname_info));
    (void)u_gettimeofday(&tv);
    (void)u_sysinfo(&sys);
    (void)u_uname(&uname_info);

    uptime_s = (tv.tv_sec > 0) ? (uint64_t)tv.tv_sec : 0ULL;
    h = uptime_s / 3600ULL;
    m = (uptime_s % 3600ULL) / 60ULL;
    s = uptime_s % 60ULL;

    ui_color_fill(ui, 0, 0, ui->fb.width, ui->fb.height, UI_BG);
    ui_color_fill(ui, 0, 0, ui->fb.width, UI_BAR_H, UI_ACCENT);
    ui_color_fill(ui, 0, UI_BAR_H, ui->fb.width, 4, UI_WHITE);
    ui_color_fill(ui, UI_MARGIN, 64, ui->fb.width - (UI_MARGIN * 2U), 102, UI_PANEL);
    ui_color_fill(ui, UI_MARGIN, 180, ui->fb.width - (UI_MARGIN * 2U), 180, UI_PANEL_DARK);
    ui_color_fill(ui, UI_MARGIN, 376, ui->fb.width - (UI_MARGIN * 2U),
                  (ui->fb.height > 400U) ? (ui->fb.height - 400U) : 0U, UI_PANEL);
    ui_color_fill(ui, 0, ui->fb.height - 4U, ui->fb.width, 4, UI_WHITE);

    ui_layout_buttons(ui);
    hovered = 0;
    show_hover_info = false;

    for (i = 0; i < (uint32_t)(UI_BUTTON_ROWS * UI_BUTTON_COLS); i++)
    {
        const struct ui_button *btn = &ui->buttons[i];
        bool hover;

        hover = ui_point_in_rect((uint32_t)ui->mouse.x, (uint32_t)ui->mouse.y, btn);
        if (hover)
        {
            hovered = i + 1U;
            show_hover_info = true;
        }
        ui_draw_button(ui, btn, hover);
    }

    ui->hovered_button = hovered;
    ui->clicked_button = 0;

    ui_color_text(ui, 32, 14, 0x00000000, UI_ACCENT, "STUPIDOS DESKTOP");
    ui_color_text(ui, 32, 92, UI_WHITE, UI_PANEL, "clickable UI launcher");
    ui_color_text(ui, 32, 118, UI_WHITE, UI_PANEL,
                  "mouse hover highlights buttons, left click launches apps or opens files");

    ui_color_text(ui, 32, 204, UI_WHITE, UI_PANEL_DARK, "system");
    ui_color_text(ui, 180, 204, UI_WHITE, UI_PANEL_DARK, uname_info.machine);
    ui_color_text(ui, 32, 230, UI_WHITE, UI_PANEL_DARK, "uptime");
    (void)snprintf(uptime_buf, sizeof(uptime_buf), "%02lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)s);
    ui_color_text(ui, 180, 230, UI_WHITE, UI_PANEL_DARK, uptime_buf);

    (void)snprintf(mem_buf, sizeof(mem_buf), "%lu / %lu MB free",
                   (unsigned long)(sys.freeram / 1024ULL / 1024ULL),
                   (unsigned long)(sys.totalram / 1024ULL / 1024ULL));
    ui_color_text(ui, 32, 256, UI_WHITE, UI_PANEL_DARK, "memory");
    ui_color_text(ui, 180, 256, UI_WHITE, UI_PANEL_DARK, mem_buf);

    (void)snprintf(info_buf, sizeof(info_buf), "pid %d  uid %d  gid %d  cwd %s",
                   u_getpid(), u_getuid(), u_getgid(), ui->cwd[0] ? ui->cwd : "/");
    ui_color_text(ui, 32, 282, UI_WHITE, UI_PANEL_DARK, "process");
    ui_color_text(ui, 180, 282, UI_WHITE, UI_PANEL_DARK, info_buf);

    (void)snprintf(cpu_buf, sizeof(cpu_buf), "%s %s", uname_info.machine, uname_info.release);
    ui_color_text(ui, 32, 308, UI_WHITE, UI_PANEL_DARK, "platform");
    ui_color_text(ui, 180, 308, UI_WHITE, UI_PANEL_DARK, cpu_buf);

    if (show_hover_info)
    {
        const struct ui_button *btn = &ui->buttons[ui->hovered_button - 1U];
        (void)snprintf(info_buf, sizeof(info_buf), "hover: %s", btn->label);
    }
    else
    {
        (void)snprintf(info_buf, sizeof(info_buf), "hover: none");
    }
    ui_color_text(ui, 32, 338, UI_WHITE, UI_PANEL_DARK, info_buf);

    if (ui->show_files)
    {
        ui_draw_file_panel(ui);
    }
    if (ui->show_network)
    {
        ui_draw_network_panel(ui);
    }

    (void)snprintf(info_buf, sizeof(info_buf), "keys: q quit  r refresh  enter launch hovered  mouse click buttons  status: %s",
                   ui->status[0] ? ui->status : "ready");
    ui_color_text(ui, 32, ui->fb.height - 48U, UI_WHITE, UI_ACCENT2, info_buf);
    ui_draw_cursor(ui, ui_clamp_u32((uint32_t)ui->mouse.x, 0U, ui->fb.width - 1U),
                   ui_clamp_u32((uint32_t)ui->mouse.y, 0U, ui->fb.height - 1U));
}

static int ui_poll_key(int timeout_ms)
{
    struct pollfd pfd;
    int ret;
    unsigned char ch;

    pfd.fd = STUPIDOS_STDIN_FILENO;
    pfd.events = POLLIN;
    pfd.revents = 0;
    ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0)
    {
        return ret;
    }

    if (!(pfd.revents & POLLIN))
    {
        return 0;
    }

    ret = (int)read(STUPIDOS_STDIN_FILENO, &ch, 1);
    if (ret <= 0)
    {
        return 0;
    }

    return (int)ch;
}

static void ui_sync_mouse(struct ui_state *ui)
{
    struct stupidos_mouseinfo mouse;

    if (!ui)
    {
        return;
    }

    u_memset(&mouse, 0, sizeof(mouse));
    if (u_mouseinfo(&mouse) == 0)
    {
        ui->mouse = mouse;
        ui->mouse_valid = true;
        return;
    }

    ui->mouse_valid = false;
}

static void ui_run_action(struct ui_state *ui, enum ui_action action)
{
    const int8_t *browser_argv[] = { (const int8_t *)"/bin/browser", (const int8_t *)"http://10.0.2.2", NULL };
    int status;
    int pid;

    if (!ui)
    {
        return;
    }

    switch (action)
    {
    case UI_ACTION_SHELL:
        ui_set_status(ui, "launching shell");
        ui_launch_child(ui, "/bin/sh", "/bin/sh", "shell");
        break;
    case UI_ACTION_BROWSER:
        ui_set_status(ui, "launching browser");
        pid = u_exec((const int8_t *)"/bin/browser", 2, browser_argv);
        if (pid < 0)
        {
            ui_set_status(ui, "browser launch failed");
            break;
        }
        (void)u_waitpid_status((int32_t)pid, &status);
        (void)snprintf(ui->status, sizeof(ui->status), "browser exited status %d", status);
        break;
    case UI_ACTION_FILES:
        ui->show_files = !ui->show_files;
        if (ui->show_files)
        {
            ui_refresh_files(ui);
            ui_set_status(ui, "file panel opened");
        }
        else
        {
            ui_set_status(ui, "file panel closed");
        }
        break;
    case UI_ACTION_NETWORK:
        ui->show_network = !ui->show_network;
        ui_set_status(ui, ui->show_network ? "network panel opened" : "network panel closed");
        break;
    case UI_ACTION_REFRESH:
        ui_copy_cwd(ui);
        ui_refresh_files(ui);
        ui_set_status(ui, "desktop refreshed");
        break;
    case UI_ACTION_QUIT:
        ui_set_status(ui, "quit requested");
        break;
    default:
        break;
    }
}

static bool ui_handle_mouse_interaction(struct ui_state *ui, bool *running)
{
    bool left_pressed;
    bool left_prev;
    enum ui_action action;
    uint32_t btn_index;

    if (!ui || !running)
    {
        return false;
    }

    left_pressed = (ui->mouse.buttons & 0x1U) != 0U;
    left_prev = (ui->prev_mouse.buttons & 0x1U) != 0U;
    action = ui_hit_test_button(ui, ui_clamp_u32((uint32_t)ui->mouse.x, 0U, ui->fb.width ? ui->fb.width - 1U : 0U),
                                ui_clamp_u32((uint32_t)ui->mouse.y, 0U, ui->fb.height ? ui->fb.height - 1U : 0U),
                                &btn_index);

    if (left_pressed && !left_prev)
    {
        ui->clicked_button = btn_index + 1U;
        if (action != UI_ACTION_NONE)
        {
            ui_run_action(ui, action);
            if (action == UI_ACTION_QUIT)
            {
                *running = false;
            }
        }
        else if (ui->show_files)
        {
            /*
             * 点击文件面板条目：
             * - 目录：进入目录
             * - 普通文件：尝试按 ELF 执行
             * 这里按最常见的使用习惯做一个轻量文件管理器。
             */
            uint32_t panel_x;
            uint32_t panel_y;
            uint32_t panel_w;
            uint32_t panel_h;
            uint32_t i;
            uint32_t line_y;

            panel_x = UI_MARGIN;
            panel_y = UI_BAR_H + 190U;
            panel_w = (ui->fb.width / 2U) - UI_MARGIN;
            panel_h = ui->fb.height - panel_y - UI_FOOTER_H - 24U;
            line_y = panel_y + 64U;

            for (i = 0; i < ui->file_count; i++)
            {
                const struct ui_file_item *it = &ui->files[i];
                uint32_t row_y;

                if (!it->valid)
                {
                    continue;
                }

                row_y = line_y;
                if ((uint32_t)ui->mouse.y >= row_y && (uint32_t)ui->mouse.y < (row_y + 22U) &&
                    (uint32_t)ui->mouse.x >= panel_x && (uint32_t)ui->mouse.x < (panel_x + panel_w))
                {
                    ui_open_file_item(ui, it);
                    break;
                }

                line_y += 22U;
                if (line_y + 24U >= panel_y + panel_h)
                {
                    break;
                }
            }
        }
    }

    ui->prev_mouse = ui->mouse;
    return left_pressed && !left_prev;
}

int main(int argc, char **argv)
{
    int key;
    bool running;
    bool mouse_changed;
    uint32_t old_mouse_x;
    uint32_t old_mouse_y;
    uint32_t old_mouse_buttons;

    (void)argc;
    (void)argv;

    memset(&ui_global, 0, sizeof(ui_global));
    ui_global.have_fb = (u_fbinfo(&ui_global.fb) == 0 && ui_global.fb.width > 0U && ui_global.fb.height > 0U);
    ui_enable_raw_terminal(&ui_global);
    atexit(ui_restore_terminal_atexit);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    ui_init_buttons(&ui_global);
    ui_copy_cwd(&ui_global);
    ui_refresh_files(&ui_global);
    ui_global.show_files = true;
    ui_global.show_network = false;
    ui_set_status(&ui_global, "desktop ready");
    ui_sync_mouse(&ui_global);
    ui_global.prev_mouse = ui_global.mouse;

    if (ui_global.have_fb)
    {
        ui_draw_frame(&ui_global);
    }
    else
    {
        ui_draw_terminal_view(&ui_global, "framebuffer unavailable, using terminal fallback");
    }

    running = true;
    while (running)
    {
        old_mouse_x = (uint32_t)ui_global.mouse.x;
        old_mouse_y = (uint32_t)ui_global.mouse.y;
        old_mouse_buttons = ui_global.mouse.buttons;
        key = ui_poll_key(UI_REFRESH_MS);
        ui_sync_mouse(&ui_global);
        mouse_changed = (old_mouse_x != (uint32_t)ui_global.mouse.x) ||
                        (old_mouse_y != (uint32_t)ui_global.mouse.y) ||
                        (old_mouse_buttons != ui_global.mouse.buttons);

        if (key == 'q' || key == 'Q')
        {
            running = false;
        }
        else if (key == 'r' || key == 'R' || key == '\n' || key == '\r')
        {
            if (key == '\n' || key == '\r')
            {
                if (ui_global.hovered_button > 0U &&
                    ui_global.hovered_button <= (uint32_t)(UI_BUTTON_ROWS * UI_BUTTON_COLS))
                {
                    ui_run_action(&ui_global, ui_global.buttons[ui_global.hovered_button - 1U].action);
                }
                else
                {
                    ui_run_action(&ui_global, UI_ACTION_REFRESH);
                }
            }
            else
            {
                ui_run_action(&ui_global, UI_ACTION_REFRESH);
            }
        }
        else if (key == 'h' || key == 'H')
        {
            ui_set_status(&ui_global, "h help | q quit | r refresh | enter launch hovered | mouse click buttons");
        }

        if (ui_global.have_fb)
        {
            if (key != 0 || mouse_changed)
            {
                (void)ui_handle_mouse_interaction(&ui_global, &running);
                ui_draw_frame(&ui_global);
            }
        }
        else if (key != 0)
        {
            ui_draw_terminal_view(&ui_global, ui_global.status[0] ? ui_global.status : 0);
        }
        else if (mouse_changed)
        {
            ui_draw_terminal_view(&ui_global, 0);
        }
    }

    ui_restore_terminal(&ui_global);
    return 0;
}
