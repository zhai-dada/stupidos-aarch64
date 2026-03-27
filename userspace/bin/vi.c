#include "libbb.h"

/*
 * BusyBox upstream 的 vi 实现。
 *
 * 这里直接复用 third_party/busybox/editors/vi.c，
 * 但把它依赖的 libbb 接口映射到 stupidos 的最小兼容层，
 * 这样就可以彻底替换掉旧的 neatvi 实现。
 */

struct globals;
struct globals *ptr_to_globals;

#include "../../third_party/busybox/editors/vi.c"

static struct termios vi_saved_termios;
static int vi_saved_termios_valid;

static void vi_restore_terminal(void)
{
    if (!vi_saved_termios_valid)
    {
        return;
    }

    /*
     * vi/vim 属于典型的全屏编辑器，最好明确由它自己控制终端模式。
     * 这里先恢复到进入 vi 之前的状态，避免外层 shell 的 raw 模式残留。
     */
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &vi_saved_termios);
    vi_saved_termios_valid = 0;
}

int main(int argc, char **argv)
{
    struct termios raw;

    (void)atexit(vi_restore_terminal);

    if (tcgetattr(STDIN_FILENO, &vi_saved_termios) == 0)
    {
        raw = vi_saved_termios;
        raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON | IXOFF);
        raw.c_oflag &= (tcflag_t)~OPOST;
        raw.c_cflag |= (tcflag_t)(CS8 | CREAD);
        raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0)
        {
            vi_saved_termios_valid = 1;
        }
    }

    (void)setvbuf(stdout, NULL, _IONBF, 0);
    (void)setvbuf(stderr, NULL, _IONBF, 0);

    return vi_main(argc, argv);
}
