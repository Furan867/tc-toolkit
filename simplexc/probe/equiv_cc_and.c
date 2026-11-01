/* equiv_cc_and.c —— `work/hook/hook_src_040.si`（**元件工坊里的与门**，4 个组件）的 C 等价物。
 *
 * 用途
 * ----
 * 同会话闭环的产物：`tools/equiv_build.py` 从捕获源码生成地址头（`addrs.h`）→ 编这个文件 →
 * 装配成 blob → 宿主里与参考产物逐 tick 对账。地址**不写死在这里**（每个游戏进程实例都不同），
 * 一律来自 `addrs.h`；编译时必须带 `-I <workdir>`（`equiv_build.py` 会传）。
 *
 * 与 `probe/006_equiv.c` 的关系：框架（协议、时钟、爆发、睡眠）逐行相同，只有电路不同 ——
 * 040 的组件是 `com_cc_input`(8 位) ×2 → `com_and_bit` → `com_cc_output`(1 位)，
 * 且元件工坊的输入端口没有信号来源（`#if in_scope("get_input")` 在这一份里不生成代码），
 * 所以两个输入只可能是 `input_replay[0]`/`input_replay[1]` 的值。
 *
 * 写法约束（为什么这么绕）
 * ----------------------
 * · **不用 CRT**：这个目标文件会被 `tcblob` 链进 blob，`memcpy`/`windows.h` 都是 `ExternResolver`
 *   解析不了的未定义符号。内存清零自己写循环。
 * · **游戏内存一律 `volatile`**：命令块由游戏（宿主）在另一个线程里改，编译器不许把
 *   `get_command(ctl_command_id)` 提出来一次算完 —— 那会让轮询永远看不到新命令。
 * · 默认只有两个 kernel32 依赖（`Sleep` / `GetSystemTimePreciseAsFileTime`，裸名 extern）+
 *   `rt_min.o` 的 `thread_exit`。参考产物走的是同一组函数的 DLL thunk。
 *
 * 编译（clang 必须来自 build/llvm/bin，不许 g++ 顶替）
 * --------------------------------------------------
 *   build/llvm/bin/clang.exe --target=x86_64-pc-windows-gnu -O2 -ffreestanding \
 *       -fno-stack-protector -c -I <workdir> -o equiv_cc_and.o equiv_cc_and.c
 *   build/tcblob.exe --obj equiv_cc_and.o --obj build/objs/entry_shim.o \
 *       --obj build/objs/rt_min.o --entry __tc_entry --out ours.bin
 *
 * `-DTC_MARK`：进入 `tc_main` 时往固定路径追加一行（游戏内验证「我们的产物真的跑到了」）。
 * **默认不编进去** —— 标记会进 extern 序列，破坏与参考产物的「逐 tick 全等」。
 */
#include "addrs.h"                 /* 由 simplexc/tools/gen_addrs.py 从本次会话的 .si 生成（生成物不入库） */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed long long i64;

/* ---- 游戏内存指针（宏来自 addrs.h）---- */
static volatile u64 *const commands = (volatile u64 *)A_COMMANDS;
static volatile u64 *const settings = (volatile u64 *)A_SETTINGS;
static volatile u64 *const input_replay = (volatile u64 *)A_INPUT_REPLAY;
static volatile u64 *const ctl_input_replay_reset = (volatile u64 *)A_CTL_RESET;
static volatile u8 *const error_buffer = (volatile u8 *)A_ERROR_BUFFER;
static volatile u8 *const sim_state = (volatile u8 *)A_SIM_STATE;
static volatile u8 *const keyboard_char = (volatile u8 *)A_KB_CHAR;
static volatile u8 *const keyboard_coord = (volatile u8 *)A_KB_COORD;

/* ---- 枚举序号（按 040 源码的声明顺序从 0 起，与宿主 simspec 的取值一致）---- */
enum {
    CMD_RUN = 0, CMD_REFRESH = 1, CMD_MODE_RESET = 2, CMD_QUIT_SIMULATION = 3
};
enum {
    CTL_COMMAND = 0, CTL_COMMAND_ID = 1, CTL_CYCLE_SPEED_MS = 2, CTL_EXIT = 3,
    CTL_LEVEL_MANUAL_INPUT = 4, CTL_LEVEL_MANUAL_INPUT_ID = 5, CTL_TEST = 6
};
enum {
    ST_SIM_CYCLE = 0, ST_SIM_TARGET_CYCLE = 1, ST_SIM_TEST_RESULT = 2,
    ST_SIM_LAST_COMMAND_ID = 3, ST_SIM_ERROR_COMPONENT = 4,
    ST_SC_COMPONENT_ID_1 = 5, ST_SC_COMPONENT_ID_2 = 6,
    ST_SIM_RUNNING = 14
};
enum { TR_PASS = 0, TR_WIN = 1, TR_FAIL = 2 };

#define CYCLE_PAST_FAIL 0ULL
#define SPEED_CAP       10000000000000ULL          /* 10 ghz，防溢出 */
#define FILE_TIME_UNIX  116444736000000000ULL      /* 1970-01-01 的 FILETIME（100ns） */

/* 程序自己的状态。参考产物把这些放在 compile_real.dll 的 .bss（宿主不比对那块），
   我们放自己的 .bss 即可：内容与地址都不同，但都不是「游戏可见状态」。
   040 没有 `Input{input}`/`Output{output,output_is_z}` 那两个字段（元件工坊的端口由游戏自己的
   UI 层持有），所以这里没有 level_* 变量。 */
static i64 cycle = -1;
static u64 last_command_id = 0;
static u64 last_level_manual_input = 0;
static u64 time_component_last = 0;

extern void thread_exit(void);
/* 两个 kernel32 依赖。x86_64 只有一个调用约定，不需要 `__stdcall` 之类的修饰；
   这里必须是**裸名 extern**（不能带 `dllimport`）—— `dllimport` 会变成 `__imp_Sleep`，
   而装配器给 `__imp_*` 造的是 8 字节数据槽、给裸名造 `movabs rax,imm64 ; jmp rax` 代码桩。 */
extern void GetSystemTimePreciseAsFileTime(u64 *ft);
extern void Sleep(u32 ms);

/* ---- 仿真协议的两个访问器（040.si 里就是这两行）---- */
static u64 get_command(u64 idx) { return commands[idx]; }
static u64 get_setting(u64 idx) { return settings[idx]; }
static void set_setting(u64 idx, u64 value) { settings[idx] = value; }

static void memory_clear(volatile u8 *dst, u64 n) {
    for (u64 i = 0; i < n; i++) dst[i] = 0;
}

static void set_error(const char *text) {
    u64 len = 0;
    while (text[len] != 0) len++;
    *(volatile u16 *)error_buffer = (u16)len;          /* 长度写在缓冲头 2 字节 */
    for (u64 i = 0; i < len; i++) error_buffer[i + 2] = (u8)text[i];
}

static u64 get_time(void) {
    u64 ft = 0;
    GetSystemTimePreciseAsFileTime(&ft);
    /* `Time` 的单位是**纳秒**（不是毫秒）—— 判据与旁证见 `probe/006_equiv.c` 同名函数。*/
    return (ft - FILE_TIME_UNIX) * 100ULL;             /* FILETIME = 100ns 单位 → ×100 = ns */
}

static void sleep_ms(u64 ms) { Sleep((u32)ms); }       /* `1.ms()` = 10^6 ns → Sleep(1) */

#ifdef TC_MARK
/* 游戏内验证用：产物真的跑到 `tc_main` 时留一行痕迹。
 *
 * 为什么无条件调 `WriteFile`/`CloseHandle`（不判 INVALID_HANDLE_VALUE 就返回）：宿主的
 * `CreateFileA` 桩恒返回 INVALID_HANDLE_VALUE（真开文件会引入真实磁盘/时间，见 host.py 的 Detours），
 * 而我们正是靠宿主记下的 `WriteFile` 记录来证明「标记路径被执行、内容是这一行」。
 * 游戏内拿到的是有效句柄，这三个调用就是一次普通的追加写。
 * 路径用相对路径：离线宿主跑时 cwd = 仓库根；游戏内跑时会落到游戏目录旁边，
 * 那里只是留一行痕迹，不影响验证（真需要换位置用 `-D MARK_PATH=...`）。 */
#define MARK_PATH "work/tmp/equiv/equiv_cc_and.log"
#define MARK_ACCESS     0x00000004u                    /* FILE_APPEND_DATA */
#define MARK_SHARE      0x00000001u                    /* FILE_SHARE_READ */
#define MARK_OPEN_ALWAYS 4u
#define MARK_ATTR       0x00000080u                    /* FILE_ATTRIBUTE_NORMAL */

/* 这三个必须是**裸名** extern，不能带 `dllimport`：`__imp_X` 会走装配器的数据槽，而槽地址是
   「遇到第一个引用时」定下来的（`assemble.cc` 的 `place_for`），之后再往代码桩块里追加就把已记下的
   槽地址挤歪了 —— 实测标记版：sidecar 说槽在 `ae0/ae8/af0`，真身在 `afc/b04/b0c`（差一个桩块），
   产物里 `mov rax,[rip+槽]` 的位移也跟着歪，宿主跑它在 entry+0x14E 崩。裸名走代码桩（只往后追加），
   地址不会再动。详见 `docs/notes/2026-10-17-equiv-cc-and.md`。 */
extern void *CreateFileA(const char *path, u32 access, u32 share, void *sa,
                         u32 disposition, u32 flags, void *tmpl);
extern int WriteFile(void *h, const void *buf, u32 n, u32 *written, void *ov);
extern int CloseHandle(void *h);

static void mark_reached(void) {
    char line[64];
    u64 n = 0;
    const char *prefix = "equiv_cc_and tc_main, sim_state=";
    while (*prefix) line[n++] = *prefix++;
    char digits[24];
    u64 d = 0;
    u64 v = (u64)(unsigned long long)A_SIM_STATE;
    if (v == 0) digits[d++] = '0';
    while (v) { digits[d++] = (char)('0' + (v % 10)); v /= 10; }
    while (d) line[n++] = digits[--d];
    line[n++] = '\r';
    line[n++] = '\n';

    void *h = CreateFileA(MARK_PATH, MARK_ACCESS, MARK_SHARE, 0, MARK_OPEN_ALWAYS, MARK_ATTR, 0);
    u32 wrote = 0;
    WriteFile(h, line, (u32)n, &wrote, 0);
    CloseHandle(h);
}
#endif

static void reset_sim(void) {
    time_component_last = 0;
    set_error("");
    /* `global_seed = Seed(get_command(ctl_test) + 1)` 落在 DLL 的 .bss；040 不用随机数，
       且宿主不比对那块 → 不复刻。*/
    last_level_manual_input = 0;
    set_setting(ST_SIM_CYCLE, ~0ULL);                 /* #U64_HIGH = -1 */
    set_setting(ST_SC_COMPONENT_ID_1, 0);
    set_setting(ST_SC_COMPONENT_ID_2, 0);
    set_setting(ST_SIM_TEST_RESULT, TR_PASS);
    cycle = -1;
    memory_clear(keyboard_char, 8192);
    memory_clear(keyboard_coord, 8192);
    memory_clear(sim_state, 264);                     /* 040 是 264，不是关卡那份的 263 */
}

static void mode_refresh(void) {
    /* 2 com_cc_input（8 位）：两位引脚都从 `input_replay[1]` 取整字节 */
    u64 value_id256 = input_replay[1];
    sim_state[256] = (u8)value_id256;
    sim_state[257] = (u8)value_id256;
    /* 1 com_cc_input（8 位）：同上，取 `input_replay[0]` */
    u64 value_id258 = input_replay[0];
    sim_state[258] = (u8)value_id258;
    sim_state[259] = (u8)value_id258;
    /* 4 com_and_bit：两个输入都取**低 1 位**，而且是从**内存**里读回刚写的字节
       （参考产物也是回读，不是复用局部变量）*/
    u8 value_id260 = (u8)((sim_state[258] & 1) & (sim_state[256] & 1));
    sim_state[260] = value_id260;
    sim_state[261] = value_id260;
    /* 3 com_cc_output（1 位）：040 的生成代码里是空的（元件工坊的端口由游戏自己的 UI 层持有），
       所以与门的结果到此为止，没有写进任何游戏内存。 */
}

static void mode_run(i64 target_cycle) {
    if (get_setting(ST_SIM_TEST_RESULT) == TR_WIN) return;
    if (get_setting(ST_SIM_TEST_RESULT) == TR_FAIL
        && cycle + 1 >= (i64)CYCLE_PAST_FAIL) return;
    if (get_setting(ST_SC_COMPONENT_ID_1) != 0) return;

    u8 halt_run = 0;
    u64 original_start_time = 0;
    u64 orig_start_cycle = (u64)cycle;
    u64 last_time = original_start_time;
    u64 burst_cycles = 1;
    u64 nanos_per_cycle = 0;
    u64 speed = 1;
    u64 last_target_speed = 0;

    while (get_command(CTL_COMMAND_ID) == last_command_id && !halt_run
           && target_cycle > cycle) {
        u64 target_speed = get_command(CTL_CYCLE_SPEED_MS);
        if (target_speed < 1) target_speed = 1;
        if (target_speed != last_target_speed) {
            original_start_time = get_time();
            orig_start_cycle = (u64)cycle;
            last_time = original_start_time;
            u64 requested = get_command(CTL_CYCLE_SPEED_MS);
            speed = requested < SPEED_CAP ? requested : SPEED_CAP;
            last_target_speed = target_speed;
        }

        i64 burst_target = cycle + (i64)burst_cycles;
        i64 burst_target_cycle = target_cycle < burst_target ? target_cycle : burst_target;
        /* 一个周期的电路求值：两个 8 位输入端口 + 一个与门。
           `get_input`/`check_output` 都不在 scope（040.si 里是 `#if in_scope(...)`），
           所以这里只读 `input_replay`；与门结果没有任何落点（com_cc_output 是空块），
           即这一段对游戏状态没有可观测副作用 —— 参考产物那边同理（差值仍然是 0）。*/
        while (cycle < burst_target_cycle) {
            u8 vid256 = (u8)input_replay[1];
            u8 vid258 = (u8)input_replay[0];
            u8 vid260 = (u8)((vid258 & 1) & (vid256 & 1));
            (void)vid260;
            cycle += 1;
        }

        set_setting(ST_SIM_CYCLE, (u64)cycle);

        u64 time_now = get_time();
        u64 time_diff = time_now - last_time;
        nanos_per_cycle = time_diff / burst_cycles;
        /* `33.ms()` = 33 × 10^6 **纳秒**（Time 的单位是 ns，`.ms()` 是换算不是后缀）*/
        if (time_now - last_time > 33 * 1000000ULL) {
            burst_cycles = burst_cycles > 1 ? burst_cycles - 1 : 1;
        } else {
            burst_cycles += 1;
        }
        last_time = time_now;

        /* 跑得比时钟快就睡一会儿 —— 宿主里永远不会进（速度封顶让目标周期远超实际周期）*/
        u64 run_target_cycle = orig_start_cycle
            + ((time_now - original_start_time) / 1000000ULL) * speed / 1000000ULL;
        while ((i64)run_target_cycle < cycle
               && get_command(CTL_COMMAND_ID) == last_command_id) {
            sleep_ms(1);
            cycle -= 1;
            mode_refresh();
            cycle += 1;
            run_target_cycle = orig_start_cycle
                + ((get_time() - original_start_time) / 1000000ULL) * speed / 1000000ULL;
        }
    }
}

static void run_sim(void) {
    for (;;) {
        reset_sim();
        /* `get_input`/`check_output` 是空函数体、且 `in_scope(...)` 为假 → 不生成代码；
           `on_manual_input` 那块同样不生成 → `manual_input_seen` 永远是 false，不实现。*/
        for (;;) {
            mode_refresh();

            while (get_command(CTL_COMMAND_ID) == last_command_id) sleep_ms(1);

            last_command_id = get_command(CTL_COMMAND_ID);   /* 先读命令号，再读命令（防竞争）*/
            set_setting(ST_SIM_LAST_COMMAND_ID, last_command_id);
            u64 command = get_command(CTL_COMMAND);

            if (command == CMD_RUN) {
                i64 target_cycle = (i64)get_setting(ST_SIM_TARGET_CYCLE);
                set_setting(ST_SIM_RUNNING, 1);
                mode_run(target_cycle);
                set_setting(ST_SIM_RUNNING, 0);
            } else if (command == CMD_REFRESH) {
                /* 空 */
            } else if (command == CMD_MODE_RESET) {
                for (u64 i = 0; i < 1024; i++) input_replay[i] = ctl_input_replay_reset[i];
                break;                                       /* 回外层重跑 reset_sim */
            } else if (command == CMD_QUIT_SIMULATION) {
                return;                                      /* break run_sim：交给收尾清理 */
            }
        }
    }
}

void tc_main(void) {
#ifdef TC_MARK
    mark_reached();
#endif
    run_sim();
    thread_exit();
}
